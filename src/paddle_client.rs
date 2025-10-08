use serde::Deserialize;
use std::sync::mpsc::Sender;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixStream;

const SOCKET_FILE: &str = "/tmp/paddle_socket_unix";

#[derive(Debug)]
pub struct OcrResult {
    pub text: String,
    pub coordinates: Vec<[f32; 2]>,
}

#[derive(Deserialize, Debug)]
struct RawOcrData {
    #[serde(rename = "rec_texts")]
    texts: Vec<String>,
    #[serde(rename = "rec_polys")]
    coordinates: Vec<Vec<[f32; 2]>>,
}

#[derive(Deserialize, Debug)]
struct RawResponse {
    res: RawOcrData,
}

pub type OcrResponse = Vec<OcrResult>;

#[derive(Debug, Clone)]
pub struct PaddleClient;

impl PaddleClient {
    pub fn new() -> Self {
        PaddleClient
    }

    pub async fn recognize(&self, image_bytes: Vec<u8>, sender: Sender<OcrResponse>) {
        if let Err(e) = self.recognize_internal(image_bytes, sender.clone()).await {
            let error_msg = format!("[PaddleClient] Error: {}", e);
            let vec_err: OcrResponse = Vec::new();
            eprintln!("{}", error_msg);
            let _ = sender.send(vec_err);
        }
    }

    async fn recognize_internal(
        &self,
        image_bytes: Vec<u8>,
        sender: Sender<OcrResponse>,
    ) -> Result<(), Box<dyn std::error::Error>> {
        let mut stream =
            match UnixStream::connect(SOCKET_FILE).await {
                Ok(s) => s,
                Err(e) => return Err(format!(
                    "No se pudo conectar al socket '{}': {}. ¿Está el servidor Python corriendo?",
                    SOCKET_FILE, e
                )
                .into()),
            };

        let len_header = (image_bytes.len() as u64).to_be_bytes();
        stream.write_all(&len_header).await?;

        stream.write_all(&image_bytes).await?;
        stream.flush().await?;

        let mut response_len_header = [0u8; 8];
        stream.read_exact(&mut response_len_header).await?;
        let response_size = u64::from_be_bytes(response_len_header) as usize;

        if response_size == 0 {
            let vec_err: OcrResponse = Vec::new();
            eprintln!("El servidor regreso vacio");
            let _ = sender.send(vec_err);
            return Ok(());
        }

        let mut response_body = vec![0u8; response_size];
        stream.read_exact(&mut response_body).await?;
        let response_str = String::from_utf8(response_body)?;

        let raw_response: RawResponse = serde_json::from_str(&response_str)?;

        let results: Vec<OcrResult> = raw_response
            .res
            .texts
            .into_iter()
            .zip(raw_response.res.coordinates.into_iter())
            .map(|(text, coordinates)| OcrResult { text, coordinates })
            .collect();

        sender.send(results)?;

        Ok(())
    }
}
