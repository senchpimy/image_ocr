use arboard::Clipboard;
use eframe::egui;
use image::ImageEncoder;
use image::{DynamicImage, RgbaImage};
use lazy_static::lazy_static;
use libwayshot::WayshotConnection;
use rusty_tesseract::{Args, Image as TessImage};
use std::sync::mpsc::{self, Receiver, TryRecvError};
use tokio::runtime::Runtime;

mod ollama;

lazy_static! {
    static ref TOKIO_RUNTIME: Runtime = Runtime::new().expect("Failed to create Tokio runtime");
}

trait RectExt {
    fn normalized(&self) -> Self;
}

impl RectExt for egui::Rect {
    fn normalized(&self) -> Self {
        let min_x = self.min.x.min(self.max.x);
        let min_y = self.min.y.min(self.max.y);
        let max_x = self.min.x.max(self.max.x);
        let max_y = self.min.y.max(self.max.y);
        egui::Rect::from_min_max(egui::pos2(min_x, min_y), egui::pos2(max_x, max_y))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Default)]
enum DragMode {
    #[default]
    None,
    Creating,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
}

#[derive(Debug, Clone)]
struct OcrWord {
    text: String,
    confidence: f32,
    bbox: egui::Rect,
}

struct ScreenshotApp {
    screenshot_image: RgbaImage,
    texture_handle: egui::TextureHandle,
    selection: Option<egui::Rect>,
    drag_start: Option<egui::Pos2>,
    drag_mode: DragMode,
    ocr_results: Vec<OcrWord>,
    ollama: ollama::OllamaClient,
    results: String,
    is_ai_working: bool,
    ai_result_receiver: Option<Receiver<String>>,
    tesseract_args: Args,
    tesseract_langs: std::vec::Vec<String>,
    clipboard: Clipboard,
}

impl ScreenshotApp {
    fn new(cc: &eframe::CreationContext<'_>, image: RgbaImage) -> Self {
        let color_image = egui::ColorImage::from_rgba_unmultiplied(
            [image.width() as usize, image.height() as usize],
            &image,
        );
        let texture_handle = cc.egui_ctx.load_texture(
            "screenshot-texture",
            color_image,
            egui::TextureOptions::LINEAR,
        );
        let clipboard = Clipboard::new().unwrap();

        let tesseract_args = Args {
            lang: "eng".to_string(),
            psm: Some(6),
            oem: Some(3),
            dpi: Some(150),
            ..Default::default()
        };
        let tesseract_langs = rusty_tesseract::get_tesseract_langs().unwrap_or_default();
        Self {
            screenshot_image: image,
            texture_handle,
            selection: None,
            drag_start: None,
            drag_mode: DragMode::default(),
            ocr_results: Vec::new(),
            ollama: ollama::OllamaClient::new(),
            results: String::new(),
            is_ai_working: false,
            ai_result_receiver: None,
            tesseract_args,
            tesseract_langs,
            clipboard,
        }
    }

    fn start_image_recognition_with_ai(&mut self) {
        if self.is_ai_working {
            return;
        }
        if let Some(selection_rect) = self.selection {
            let sel = selection_rect.normalized();
            let x = sel.min.x.round() as u32;
            let y = sel.min.y.round() as u32;
            let width = sel.width().round() as u32;
            let height = sel.height().round() as u32;

            if width == 0 || height == 0 {
                return;
            }

            let cropped_rgba =
                image::imageops::crop_imm(&self.screenshot_image, x, y, width, height).to_image();
            let mut image_bytes: Vec<u8> = Vec::new();
            let encoder = image::codecs::png::PngEncoder::new(&mut image_bytes);
            if encoder
                .write_image(
                    &cropped_rgba,
                    cropped_rgba.width(),
                    cropped_rgba.height(),
                    image::ColorType::Rgba8.into(),
                )
                .is_err()
            {
                self.results = "Error: No se pudo codificar la imagen a PNG.".to_string();
                return;
            }

            let (sender, receiver) = mpsc::channel();
            self.ai_result_receiver = Some(receiver);
            self.is_ai_working = true;
            self.results = "Analizando imagen con IA...".to_string();
            let ollama_clone = self.ollama.clone();
            let owned_image_bytes = image_bytes.clone();
            let owned_sender = sender.clone();

            TOKIO_RUNTIME.spawn(async move {
                ollama_clone
                    .generate_stream(owned_image_bytes, owned_sender)
                    .await;
            });
        }
    }

    fn poll_ai_result(&mut self) {
        if let Some(receiver) = &self.ai_result_receiver {
            for chunk in receiver.try_iter() {
                if self.results == "Analizando imagen con IA..." {
                    self.results.clear();
                }
                self.results.push_str(&chunk);
            }
            if let Err(TryRecvError::Disconnected) = receiver.try_recv() {
                self.is_ai_working = false;
                self.ai_result_receiver = None;
            }
        }
    }

    fn perform_ocr(&mut self) {
        if let Some(selection_rect) = self.selection {
            let sel = selection_rect.normalized();
            let x = sel.min.x.round() as u32;
            let y = sel.min.y.round() as u32;
            let width = sel.width().round() as u32;
            let height = sel.height().round() as u32;

            if width > 0 && height > 0 {
                let cropped_dyn_image = DynamicImage::ImageRgba8(
                    image::imageops::crop_imm(&self.screenshot_image, x, y, width, height)
                        .to_image(),
                );
                let tesseract_image = TessImage::from_dynamic_image(&cropped_dyn_image)
                    .expect("No se pudo crear la imagen para Tesseract");

                println!("Ejecutando OCR en la selección...");
                match rusty_tesseract::image_to_data(&tesseract_image, &self.tesseract_args) {
                    Ok(data) => {
                        self.ocr_results.clear();
                        self.results.clear();
                        for line in data.output.lines().skip(1) {
                            let columns: Vec<&str> = line.split('\t').collect();
                            if columns.len() == 12 {
                                if let (Ok(confidence), Ok(x), Ok(y), Ok(w), Ok(h)) = (
                                    columns[10].parse::<f32>(),
                                    columns[6].parse::<f32>(),
                                    columns[7].parse::<f32>(),
                                    columns[8].parse::<f32>(),
                                    columns[9].parse::<f32>(),
                                ) {
                                    let text = columns[11];
                                    if confidence > 50.0 && !text.trim().is_empty() {
                                        self.results.push_str(&format!("{} ", text.trim()));
                                        self.ocr_results.push(OcrWord {
                                            text: text.to_string(),
                                            confidence,
                                            bbox: egui::Rect::from_min_size(
                                                egui::pos2(x, y),
                                                egui::vec2(w, h),
                                            ),
                                        });
                                    }
                                }
                            }
                        }
                    }
                    Err(e) => eprintln!("Error de Tesseract: {:?}", e),
                }
            }
        }
    }
}

impl eframe::App for ScreenshotApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.poll_ai_result();

        egui::CentralPanel::default()
            .frame(egui::Frame::none())
            .show(ctx, |ui| {
                ui.image((self.texture_handle.id(), ui.available_size()));
                let response = ui.interact(
                    ui.max_rect(),
                    ui.id().with("screenshot_area"),
                    egui::Sense::click_and_drag(),
                );

                let pointer_pos = response.interact_pointer_pos();

                if response.is_pointer_button_down_on() && self.drag_mode == DragMode::None {
                    if let (Some(selection), Some(pos)) = (self.selection, pointer_pos) {
                        let handle_radius = 8.0;
                        let norm_sel = selection.normalized();
                        if norm_sel.left_top().distance(pos) < handle_radius {
                            self.drag_mode = DragMode::TopLeft;
                        } else if norm_sel.right_top().distance(pos) < handle_radius {
                            self.drag_mode = DragMode::TopRight;
                        } else if norm_sel.left_bottom().distance(pos) < handle_radius {
                            self.drag_mode = DragMode::BottomLeft;
                        } else if norm_sel.right_bottom().distance(pos) < handle_radius {
                            self.drag_mode = DragMode::BottomRight;
                        } else {
                            self.drag_mode = DragMode::Creating;
                            self.drag_start = Some(pos);
                            self.selection = Some(egui::Rect::from_min_size(pos, egui::Vec2::ZERO));
                            self.ocr_results.clear();
                        }
                    } else if let Some(pos) = pointer_pos {
                        self.drag_mode = DragMode::Creating;
                        self.drag_start = Some(pos);
                        self.selection = Some(egui::Rect::from_min_size(pos, egui::Vec2::ZERO));
                        self.ocr_results.clear();
                    }
                }

                if response.dragged() {
                    if let (Some(pos), Some(selection)) = (pointer_pos, &mut self.selection) {
                        match self.drag_mode {
                            DragMode::Creating => {
                                if let Some(start_pos) = self.drag_start {
                                    *selection = egui::Rect::from_two_pos(start_pos, pos);
                                }
                            }
                            DragMode::TopLeft => {
                                *selection = egui::Rect::from_two_pos(pos, selection.right_bottom())
                            }
                            DragMode::TopRight => {
                                *selection = egui::Rect::from_two_pos(pos, selection.left_bottom())
                            }
                            DragMode::BottomLeft => {
                                *selection = egui::Rect::from_two_pos(pos, selection.right_top())
                            }
                            DragMode::BottomRight => {
                                *selection = egui::Rect::from_two_pos(pos, selection.left_top())
                            }
                            DragMode::None => {}
                        }
                    }
                }

                if response.drag_stopped() {
                    if let Some(selection) = &mut self.selection {
                        *selection = selection.normalized();
                        self.perform_ocr();
                    }
                    self.drag_mode = DragMode::None;
                    self.drag_start = None;
                }

                if let Some(selection_rect) = self.selection {
                    let selection_rect = selection_rect.normalized();
                    let painter = ui.painter();
                    let screen_rect = ui.max_rect();

                    let dark_color = egui::Color32::from_rgba_unmultiplied(0, 0, 0, 180);
                    painter.rect_filled(
                        egui::Rect::from_x_y_ranges(
                            screen_rect.x_range(),
                            screen_rect.top()..=selection_rect.top(),
                        ),
                        0.0,
                        dark_color,
                    );
                    painter.rect_filled(
                        egui::Rect::from_x_y_ranges(
                            screen_rect.x_range(),
                            selection_rect.bottom()..=screen_rect.bottom(),
                        ),
                        0.0,
                        dark_color,
                    );
                    painter.rect_filled(
                        egui::Rect::from_x_y_ranges(
                            screen_rect.left()..=selection_rect.left(),
                            selection_rect.y_range(),
                        ),
                        0.0,
                        dark_color,
                    );
                    painter.rect_filled(
                        egui::Rect::from_x_y_ranges(
                            selection_rect.right()..=screen_rect.right(),
                            selection_rect.y_range(),
                        ),
                        0.0,
                        dark_color,
                    );

                    painter.rect_stroke(
                        selection_rect,
                        0.0,
                        egui::Stroke::new(1.0, egui::Color32::LIGHT_BLUE),
                    );

                    let corner_radius = 5.0;
                    let corner_color = egui::Color32::from_rgb(255, 255, 255);
                    painter.circle_filled(selection_rect.left_top(), corner_radius, corner_color);
                    painter.circle_filled(selection_rect.right_top(), corner_radius, corner_color);
                    painter.circle_filled(
                        selection_rect.left_bottom(),
                        corner_radius,
                        corner_color,
                    );
                    painter.circle_filled(
                        selection_rect.right_bottom(),
                        corner_radius,
                        corner_color,
                    );

                    if self.drag_mode == DragMode::None {
                        if let Some(pos) = pointer_pos {
                            let handle_radius = 8.0;
                            if selection_rect.left_top().distance(pos) < handle_radius
                                || selection_rect.right_bottom().distance(pos) < handle_radius
                            {
                                ctx.set_cursor_icon(egui::CursorIcon::ResizeNwSe);
                            } else if selection_rect.right_top().distance(pos) < handle_radius
                                || selection_rect.left_bottom().distance(pos) < handle_radius
                            {
                                ctx.set_cursor_icon(egui::CursorIcon::ResizeNeSw);
                            }
                        }
                    }

                    if self.drag_mode == DragMode::None {
                        for word in &self.ocr_results {
                            let word_rect = word.bbox.size();
                            let screen_bbox = egui::Rect::from_min_size(
                                selection_rect.min + word.bbox.min.to_vec2(),
                                word_rect,
                            );
                            painter.rect_stroke(
                                screen_bbox,
                                0.0,
                                egui::Stroke::new(2.0, egui::Color32::GREEN),
                            );
                        }
                    }

                    if self.drag_mode == DragMode::None {
                        egui::Area::new("context_menu".into())
                            .fixed_pos(selection_rect.right_bottom() + egui::vec2(5.0, 5.0))
                            .show(ctx, |ui| {
                                egui::Frame::popup(ui.style()).show(ui, |ui| {
                                    ui.collapsing("Tesseract Config", |ui| {
                                        let mut selected_psm = self.tesseract_args.psm.unwrap_or(3);
                                        let mut selected_oem = self.tesseract_args.oem.unwrap_or(3);
                                        let cur_lang = &mut self.tesseract_args.lang;
                                        ui.label("Tesseract Lang");
                                        egui::ComboBox::from_id_source("lang_select")
                                            .selected_text(cur_lang.as_str())
                                            .show_ui(ui, |ui| {
                                                for lang in &self.tesseract_langs {
                                                    ui.selectable_value(
                                                        cur_lang,
                                                        lang.clone(),
                                                        lang,
                                                    );
                                                }
                                            });
                                        ui.label("PSM (Page Segmentation Mode):");
                                        egui::ComboBox::from_id_source("psm_select")
                                            .selected_text(format!("{}", selected_psm))
                                            .show_ui(ui, |ui| {
                                                for i in 0..=13 {
                                                    if ui
                                                        .selectable_value(
                                                            &mut selected_psm,
                                                            i,
                                                            format!("{}", i),
                                                        )
                                                        .changed()
                                                    {
                                                        self.tesseract_args.psm =
                                                            Some(selected_psm);
                                                    }
                                                }
                                            });
                                        ui.label("OEM (OCR Engine Mode):");
                                        egui::ComboBox::from_id_source("oem_select")
                                            .selected_text(format!("{}", selected_oem))
                                            .show_ui(ui, |ui| {
                                                for i in 0..=3 {
                                                    if ui
                                                        .selectable_value(
                                                            &mut selected_oem,
                                                            i,
                                                            format!("{}", i),
                                                        )
                                                        .changed()
                                                    {
                                                        self.tesseract_args.oem =
                                                            Some(selected_oem);
                                                    }
                                                }
                                            });
                                        let mut dpi_float =
                                            self.tesseract_args.dpi.unwrap_or(150) as f32;
                                        if ui
                                            .add(
                                                egui::Slider::new(&mut dpi_float, 50.0..=300.0)
                                                    .suffix("dpi"),
                                            )
                                            .changed()
                                        {
                                            self.tesseract_args.dpi = Some(dpi_float as i32);
                                        }
                                        if ui.button("Recognize text (Tesseract)").clicked() {
                                            self.perform_ocr();
                                        }
                                    });
                                    ui.add_enabled_ui(!self.is_ai_working, |ui| {
                                        if ui.button("Recognize with AI").clicked() {
                                            self.start_image_recognition_with_ai();
                                        }
                                    });

                                    if !self.results.is_empty() {
                                        if ui.button("📋 Copiar Texto").clicked() {
                                            //self.clipboard.set_text(self.results.clone()).unwrap();
                                            self.clipboard.set_text("AAAAA".to_string()).unwrap();
                                        }
                                    }

                                    ui.add(
                                        egui::TextEdit::multiline(&mut self.results)
                                            .font(egui::TextStyle::Monospace)
                                            .code_editor()
                                            .desired_rows(10)
                                            .lock_focus(true)
                                            .desired_width(300.),
                                    );
                                });
                            });
                    }
                }

                if ctx.input(|i| i.key_pressed(egui::Key::Escape)) {
                    ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                }
            });

        if self.is_ai_working {
            ctx.request_repaint();
        }
    }
}

fn main() -> Result<(), eframe::Error> {
    println!("Tomando captura de pantalla...");
    let wayshot_connection =
        WayshotConnection::new().expect("No se pudo conectar al servidor Wayland.");
    let screenshot = wayshot_connection
        .screenshot_all(false)
        .expect("Fallo al tomar la captura de pantalla.");
    let (width, height) = screenshot.dimensions();
    let raw_buffer = screenshot.into_raw();
    let screenshot_app_image = image::RgbaImage::from_raw(width, height, raw_buffer)
        .expect("No se pudo convertir el búfer.");
    println!("Captura tomada. Iniciando editor...");
    let native_options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_decorations(false)
            .with_fullscreen(true),
        ..Default::default()
    };
    eframe::run_native(
        "Editor de Captura con OCR",
        native_options,
        Box::new(|cc| Box::new(ScreenshotApp::new(cc, screenshot_app_image))),
    )
}
