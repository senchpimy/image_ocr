use base64::{Engine as _, engine::general_purpose::STANDARD};
use reqwest::Client;
use serde::{Deserialize, Serialize};
use std::sync::mpsc::Sender;

const API_KEY: &str = include_str!("../gemini");

#[derive(Serialize)]
struct GeminiRequest {
    contents: Vec<Content>,
}

#[derive(Serialize)]
struct Content {
    parts: Vec<Part>,
}

#[derive(Serialize)]
struct Part {
    #[serde(skip_serializing_if = "Option::is_none")]
    text: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    inline_data: Option<InlineData>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct InlineData {
    mime_type: String,
    data: String,
}

#[derive(Deserialize, Debug)]
struct GeminiResponse {
    candidates: Vec<Candidate>,
}

#[derive(Deserialize, Debug)]
struct Candidate {
    content: ResponseContent,
}

#[derive(Deserialize, Debug)]
struct ResponseContent {
    parts: Vec<ResponsePart>,
}

#[derive(Deserialize, Debug)]
struct ResponsePart {
    text: String,
}

#[derive(Debug, Clone)]
pub struct GeminiClient {
    client: Client,
    pub model: String,
    prompt: String,
}

impl GeminiClient {
    pub fn new() -> Self {
        #[cfg(feature = "gemini_translate")]
        let prompt =
            "Traduce el texto en la imagen al español, solo responde con la traducción".to_string();

        #[cfg(not(feature = "gemini_translate"))]
        let prompt = "Extrae cualquier texto visible en esta imagen. Responde únicamente con el texto extraído.".to_string();

        Self {
            client: Client::new(),
            model: "gemini-2.5-flash-lite".to_string(),
            prompt,
        }
    }

    pub async fn generate(&self, image_bytes: Vec<u8>, sender: Sender<String>) {
        let base64_image = STANDARD.encode(&image_bytes);

        let request_body = GeminiRequest {
            contents: vec![Content {
                parts: vec![
                    Part {
                        text: Some(self.prompt.clone()),
                        inline_data: None,
                    },
                    Part {
                        text: None,
                        inline_data: Some(InlineData {
                            mime_type: "image/png".to_string(),
                            data: base64_image,
                        }),
                    },
                ],
            }],
        };

        let url = format!(
            "https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent",
            self.model
        );

        match self
            .client
            .post(&url)
            .header("X-goog-api-key", API_KEY.trim())
            .header("Content-Type", "application/json")
            .json(&request_body)
            .send()
            .await
        {
            Ok(response) => {
                if response.status().is_success() {
                    match response.json::<GeminiResponse>().await {
                        Ok(gemini_response) => {
                            let text_result = gemini_response
                                .candidates
                                .get(0)
                                .and_then(|c| c.content.parts.get(0))
                                .map_or("".to_string(), |p| p.text.clone());

                            if sender.send(text_result).is_err() {
                                eprintln!("[gemini.rs] El receptor del canal se cerró.");
                            }
                        }
                        Err(e) => {
                            let error_msg = format!("[gemini.rs] Error al decodificar JSON: {}", e);
                            eprintln!("{}", error_msg);
                            let _ = sender.send(error_msg);
                        }
                    }
                } else {
                    let status = response.status();
                    let error_body = response
                        .text()
                        .await
                        .unwrap_or_else(|_| "Cuerpo del error ilegible".to_string());
                    let error_msg =
                        format!("[gemini.rs] Error de API: {} - {}", status, error_body);
                    eprintln!("{}", error_msg);
                    let _ = sender.send(error_msg);
                }
            }
            Err(e) => {
                let error_msg = format!("[gemini.rs] Error de red: {}", e);
                eprintln!("{}", error_msg);
                let _ = sender.send(error_msg);
            }
        }
    }
}
