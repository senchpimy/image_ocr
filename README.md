# OCR

<img src="https://img.icons8.com/fluency/96/screenshot.png" align="right" width="120" height="120" alt="OCR logo"/>

**OCR** es una herramienta de escritorio modular para entornos Wayland que permite tomar capturas de pantalla, seleccionar regiones, y luego reconocer y/o traducir el texto visible. Utiliza una variedad de motores, incluyendo Tesseract, PaddleOCR, y modelos de IA a través de Ollama y Gemini, todo desde una interfaz gráfica desarrollada en Rust con `egui`.

- **Captura de pantalla instantánea** en Wayland.
- **Selección de región** precisa con el cursor.
- **Múltiples motores de reconocimiento**: Compila la aplicación con el motor que necesites.
  - **Tesseract**: OCR local y configurable.
  - **PaddleOCR**: Alternativa de OCR local de alta precisión.
  - **Ollama**: Análisis de imagen con modelos de IA locales (ej. LLaVA).
  - **Gemini**: Reconocimiento y análisis con la API de Google Gemini.
- **Modo de Traducción**: Compila la aplicación para que Ollama o Gemini traduzcan directamente el texto de la imagen al español.
- **Copia el resultado** al portapapeles con un solo clic.

## ¿Por qué?

OCR combina el reconocimiento de texto tradicional con el análisis de imágenes por IA, permitiendo extraer, entender y traducir información de cualquier sección visible de tu pantalla. Su naturaleza modular te permite compilar una herramienta ligera y específica para tus necesidades, sin depender de servicios externos si no lo deseas.

## Cómo funciona

1.  Al iniciar, se toma una captura completa de pantalla usando `libwayshot`.
2.  Se abre una ventana sin bordes a pantalla completa.
3.  Seleccionas una región con el mouse.
4.  Al soltar el clic, aparece un menú contextual con las acciones disponibles según las funcionalidades con las que se compiló la aplicación. Puedes:
    - Ejecutar OCR con Tesseract.
    - Ejecutar OCR con PaddleOCR.
    - Enviar la imagen seleccionada a un modelo de Ollama (para extraer texto o traducir).
    - Enviar la imagen a la API de Gemini (para extraer texto o traducir).
5.  El texto resultante se muestra en pantalla y puede copiarse al portapapeles.

## Instalación y Compilación

### Prerrequisitos

Primero, instala las dependencias necesarias para tu sistema.

- **Generales**:
  - **Rust Toolchain**: `rustc`, `cargo`.
  - **Utilidades de Wayland**: `wl-clipboard` y librerías de desarrollo.
    - En Arch: `sudo pacman -S wl-clipboard pkg-config libxkbcommon`
    - En Debian/Ubuntu: `sudo apt install wl-clipboard pkg-config libxkbcommon-dev libgtk-3-dev`

- **Por Característica (Feature)**:
  - **`tesseract`**: Motor Tesseract OCR y datos de idioma.
    - En Arch: `sudo pacman -S tesseract tesseract-data-eng`
    - En Debian/Ubuntu: `sudo apt install tesseract-ocr tesseract-ocr-eng`
  - **`paddleocr`**: Un servicio de PaddleOCR en ejecución.
  - **`ollama` / `ollama_translate`**: El servicio [Ollama](https://ollama.com) instalado y un modelo multimodal.
    ```bash
    ollama pull #modelo
    ```
  - **`gemini` / `gemini_translate`**: Una clave de API de Google AI Studio en un archivo

### Compilación con Features

Elige qué funcionalidades incluir usando las *features* de Cargo.

| Feature            | Descripción                                                        |
| ------------------ | ------------------------------------------------------------------ |
| `tesseract`        | Habilita el reconocimiento local con Tesseract.                    |
| `paddleocr`        | Habilita el reconocimiento local con PaddleOCR.                    |
| `ollama`           | Habilita el **reconocimiento de texto** con Ollama.                |
| `ollama_translate` | **Reemplaza** el reconocimiento con **traducción** usando Ollama.  |
| `gemini`           | Habilita el **reconocimiento de texto** con Gemini.                |
| `gemini_translate` | **Reemplaza** el reconocimiento con **traducción** usando Gemini.  |
| `full`             | Habilita todas las funcionalidades.                                |

**Ejemplos:**

- **Versión solo con Tesseract:**
  ```bash
  cargo build --release --no-default-features --features "tesseract"
  ```
- **Versión para traducir con Ollama:**
  ```bash
  cargo build --release --no-default-features --features "ollama_translate"
  ```
- **Versión completa:**
  ```bash
  cargo build --release --no-default-features --features "full"
  ```

El binario se encontrará en `./target/release/ocr`.

## Tecnologías utilizadas

| Tecnología          | Descripción                                           |
| ------------------- | ----------------------------------------------------- |
| `eframe` / `egui`   | Framework para la interfaz gráfica nativa en Rust     |
| `libwayshot`        | Captura de pantalla en entornos Wayland               |
| `rusty-tesseract`   | Bindings de Rust para el motor Tesseract OCR          |
| `ollama-rs`         | Interacción con el servicio local de Ollama           |
| `tokio`             | Runtime asíncrono para gestionar las peticiones de IA |

## Licencia

MIT
