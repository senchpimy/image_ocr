import numpy as np
import socket
import struct
import cv2
import os
import json
import argparse

# Parse arguments
parser = argparse.ArgumentParser(description='Servidor OCR')
parser.add_argument('--model', type=str, choices=['paddle', 'lighton'], default='paddle', help='Modelo OCR a utilizar')
args = parser.parse_args()

# Global OCR object
ocr_engine = None

def init_ocr():
    global ocr_engine
    if args.model == 'paddle':
        from paddleocr import PaddleOCR
        print("Cargando PaddleOCR...")
        ocr_engine = PaddleOCR(
            lang="es",
            use_doc_orientation_classify=False,
            use_doc_unwarping=False,
            use_textline_orientation=False
        )
    else:
        from transformers import AutoProcessor, AutoModelForVision2Seq
        import torch
        print("Cargando LightOnOCR-2-1B...")
        model_id = "lightonai/LightOnOCR-2-1B"
        processor = AutoProcessor.from_pretrained(model_id, trust_remote_code=True)
        device = "cuda" if torch.cuda.is_available() else "cpu"
        dtype = torch.float16 if device == "cuda" else torch.float32
        model = AutoModelForVision2Seq.from_pretrained(
            model_id, 
            trust_remote_code=True,
            torch_dtype=dtype
        ).to(device)
        model.eval()
        ocr_engine = (model, processor)

def do_ocr(img_np):
    if args.model == 'paddle':
        result = ocr_engine.predict(input=img_np)
        if result and result[0] is not None:
            return result[0].json
        return {}
    else:
        model, processor = ocr_engine
        from PIL import Image
        import torch
        img_rgb = cv2.cvtColor(img_np, cv2.COLOR_BGR2RGB)
        image = Image.fromarray(img_rgb)
        inputs = processor(images=image, return_tensors="pt").to(model.device)
        with torch.no_grad():
            outputs = model.generate(**inputs, max_new_tokens=1536, do_sample=False)
        ocr_text = processor.decode(outputs[0], skip_special_tokens=True)
        return {"text": ocr_text}

init_ocr()

def recvall(sock, n):
    """Recibe exactamente 'n' bytes del socket 'sock'."""
    data = bytearray()
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data.extend(packet)
    return bytes(data)


    #ocr = PaddleOCR(
    #    lang="es",
    #    use_doc_orientation_classify=False,
    #    use_doc_unwarping=False,
    #    use_textline_orientation=False
    #)

SOCKET_FILE = '/tmp/paddle_socket_unix'
try:
    os.unlink(SOCKET_FILE)
except OSError:
    if os.path.exists(SOCKET_FILE):
        raise

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
print(f'Iniciando en {SOCKET_FILE}')
sock.bind(SOCKET_FILE)
sock.listen(5)

while True:
    connection, client_address = sock.accept()
    try:
        print('Conexión establecida.')

        while True:
            header_size = struct.calcsize(">Q")
            packed_img_size = recvall(connection, header_size)
            if not packed_img_size:
                print("El cliente cerró la conexión.")
                break

            img_size = struct.unpack(">Q", packed_img_size)[0]
            img_data = recvall(connection, img_size)
            if not img_data:
                print("la conexión se cerró.")
                break

            print(f"Recibidos {len(img_data)} bytes.")

            nparr = np.frombuffer(img_data, np.uint8)
            img_np = cv2.imdecode(nparr, cv2.IMREAD_COLOR)

            if img_np is None:
                response_data = json.dumps({"error": "IMAGEN INVALIDA"})
            else:
                try:
                    res = do_ocr(img_np)
                    response_data = json.dumps(res, ensure_ascii=False)
                    print(f"Respuesta de {args.model} enviada.")
                except Exception as e:
                    print(f"Error en OCR ({args.model}): {e}")
                    response_data = json.dumps({"error": str(e)})

            response_bytes = response_data.encode('utf-8')

            response_header = struct.pack('>Q', len(response_bytes))

            connection.sendall(response_header)

            connection.sendall(response_bytes)

    except (socket.error, ConnectionResetError) as e:
        print(f"Error de conexión: {e}")
    finally:
        connection.close()
