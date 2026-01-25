use base64::{Engine as _, engine::general_purpose::STANDARD};
use ollama_rs::{
    Ollama,
    generation::{completion::request::GenerationRequest, images::Image},
};
use std::sync::mpsc::Sender;
use tokio_stream::StreamExt;

#[derive(Debug, Clone)]
pub struct OllamaClient {
    ollama: Ollama,
    pub model: String,
    prompt: String,
}

impl OllamaClient {
    pub fn new() -> Self {
        #[cfg(feature = "ollama_translate")]
        let prompt =
            "Traduce el texto en la imagen al español, solo responde con la traducción".to_string();

        #[cfg(not(feature = "ollama_translate"))]
        let prompt = "Extrae cualquier texto visible en esta imagen. Responde únicamente con el texto extraído.".to_string();

        OllamaClient {
            ollama: Ollama::default(),
            model: "gemma3:12b".to_string(),
            prompt,
        }
    }

    pub async fn generate_stream(&self, image_bytes: Vec<u8>, sender: Sender<String>) {
        let base64_image = STANDARD.encode(&image_bytes);
        let image = Image::from_base64(&base64_image);
        let request =
            GenerationRequest::new(self.model.clone(), self.prompt.clone()).add_image(image);
        match self.ollama.generate_stream(request).await {
            Ok(mut stream) => {
                while let Some(res) = stream.next().await {
                    match res {
                        Ok(responses) => {
                            for resp in responses {
                                if sender.send(resp.response).is_err() {
                                    eprintln!(
                                        "[ollama.rs] El receptor del canal se cerró. Terminando stream."
                                    );
                                    return;
                                }
                            }
                        }
                        Err(e) => {
                            let error_msg = format!("[ollama.rs] Error en el stream: {}", e);
                            eprintln!("{}", error_msg);
                            let _ = sender.send(error_msg);
                            break;
                        }
                    }
                }
            }
            Err(e) => {
                let error_msg = format!("[ollama.rs] No se pudo iniciar el stream: {}", e);
                eprintln!("{}", error_msg);
                let _ = sender.send(error_msg);
            }
        }
    }
}
