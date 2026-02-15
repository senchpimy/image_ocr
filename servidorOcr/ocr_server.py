import numpy as np
import socket
import struct
import cv2
import os
import json
import argparse

# Parse arguments
parser = argparse.ArgumentParser(description='Servidor OCR')
parser.add_argument('--model', type=str, choices=['paddle', 'lighton', 'glm-ocr'], default='paddle', help='Modelo OCR a utilizar')
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
    elif args.model == 'lighton':
        import torch
        from transformers import LightOnOcrForConditionalGeneration, LightOnOcrProcessor
        print("Cargando LightOnOCR-2-1B...")
        model_id = "lightonai/LightOnOCR-2-1B"
        
        device = "mps" if torch.backends.mps.is_available() else "cuda" if torch.cuda.is_available() else "cpu"
        dtype = torch.float32 if device == "mps" else torch.bfloat16
        
        model = LightOnOcrForConditionalGeneration.from_pretrained(model_id, torch_dtype=dtype).to(device)
        processor = LightOnOcrProcessor.from_pretrained(model_id)
        model.eval()
        ocr_engine = (model, processor, device, dtype)
    elif args.model == 'glm-ocr':
        from transformers import AutoProcessor, AutoModelForImageTextToText
        import torch
        print("Cargando GLM-OCR...")
        model_id = "zai-org/GLM-OCR"
        
        device = "cuda" if torch.cuda.is_available() else "cpu"
        # float16 es generalmente más compatible en ROCm que bfloat16 si no es una GPU muy reciente
        dtype = torch.float16 if device == "cuda" else torch.float32
        
        processor = AutoProcessor.from_pretrained(model_id, trust_remote_code=True)
        model = AutoModelForImageTextToText.from_pretrained(
            model_id,
            torch_dtype=dtype,
            trust_remote_code=True
        ).to(device)
        model.eval()
        ocr_engine = (model, processor, device, dtype)

def do_ocr(img_np):
    if args.model == 'paddle':
        result = ocr_engine.predict(input=img_np)
        if result and result[0] is not None:
            return result[0].json
        return {}
    elif args.model == 'lighton':
        model, processor, device, dtype = ocr_engine
        from PIL import Image
        import torch
        
        img_rgb = cv2.cvtColor(img_np, cv2.COLOR_BGR2RGB)
        image = Image.fromarray(img_rgb)
        
        conversation = [{"role": "user", "content": [{"type": "image", "image": image}]}]
        
        inputs = processor.apply_chat_template(
            conversation,
            add_generation_prompt=True,
            tokenize=True,
            return_dict=True,
            return_tensors="pt",
        )
        inputs = {k: v.to(device=device, dtype=dtype) if v.is_floating_point() else v.to(device) for k, v in inputs.items()}
        
        with torch.no_grad():
            output_ids = model.generate(**inputs, max_new_tokens=1024)
        
        generated_ids = output_ids[0, inputs["input_ids"].shape[1]:]
        output_text = processor.decode(generated_ids, skip_special_tokens=True)
        
        # Return format expected by CPaddleOCR
        return {
            "rec_texts": [output_text],
            "dt_polys": [[[0, 0], [img_np.shape[1], 0], [img_np.shape[1], img_np.shape[0]], [0, img_np.shape[0]]]]
        }
    elif args.model == 'glm-ocr':
        model, processor, device, dtype = ocr_engine
        from PIL import Image
        import torch
        
        img_rgb = cv2.cvtColor(img_np, cv2.COLOR_BGR2RGB)
        image = Image.fromarray(img_rgb)
        
        messages = [
            {
                "role": "user",
                "content": [
                    {
                        "type": "image",
                        "image": image
                    },
                    {
                        "type": "text",
                        "text": "Text Recognition:"
                    }
                ],
            }
        ]
        
        inputs = processor.apply_chat_template(
            messages,
            tokenize=True,
            add_generation_prompt=True,
            return_dict=True,
            return_tensors="pt"
        ).to(device)
        
        # Asegurar que los tensores de punto flotante tengan el dtype correcto
        inputs = {k: v.to(dtype=dtype) if v.is_floating_point() else v for k, v in inputs.items()}
        inputs.pop("token_type_ids", None)
        
        with torch.no_grad():
            generated_ids = model.generate(**inputs, max_new_tokens=8192)
            
        output_text = processor.decode(generated_ids[0][inputs["input_ids"].shape[1]:], skip_special_tokens=False)
        
        return {
            "rec_texts": [output_text],
            "dt_polys": [[[0, 0], [img_np.shape[1], 0], [img_np.shape[1], img_np.shape[0]], [0, img_np.shape[0]]]]
        }

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
