# OCR

<img src="https://img.icons8.com/fluency/96/screenshot.png" align="right" width="120" height="120" alt="OCR logo"/>

**OCR** es una herramienta de escritorio para entornos Wayland que permite tomar capturas de pantalla, seleccionar regiones, reconocer texto mediante Tesseract y analizar imágenes usando modelos de inteligencia artificial vía Ollama, todo desde una interfaz gráfica desarrollada en Rust con egui.

* Captura automáticamente la pantalla completa
* Permite seleccionar cualquier región con el cursor
* Realiza OCR configurable mediante Tesseract
* Procesa imágenes con modelos de lenguaje a través de Ollama
* Copia el resultado al portapapeles

## ¿Por qué?

OCR combina el reconocimiento de texto tradicional con el análisis de imágenes por IA, permitiendo extraer y entender información de cualquier sección visible de tu pantalla sin depender de herramientas externas.

## Cómo funciona

1. Al iniciar, se toma una captura completa de pantalla usando `libwayshot`.
2. Se abre una ventana sin bordes a pantalla completa.
3. Seleccionas una región con el mouse.
4. Puedes:

   * Ejecutar OCR con Tesseract.
   * Enviar la imagen seleccionada a un modelo de Ollama.
5. El texto reconocido se muestra en pantalla y puede copiarse al portapapeles.

## Instalación

Requisitos:

* Rust (estable)
* Wayland como servidor gráfico
* `tesseract-ocr` instalado en el sistema
* [`ollama`](https://ollama.com) instalado y corriendo

```bash
cd image_ocr
cargo run --release
```

## Tecnologías utilizadas

| Tecnología        | Descripción                                  |
| ----------------- | -------------------------------------------- |
| `eframe` / `egui` | Interfaz gráfica nativa en Rust              |
| `ollama`          | Cliente personalizado para modelos de IA     |

## Licencia

MIT
