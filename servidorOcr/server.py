import numpy as np
from paddleocr import PaddleOCR
import socket
import struct
import cv2
import os
from paddlex.inference.pipelines.ocr.result import OCRResult
import json


def recvall(sock, n):
    """Recibe exactamente 'n' bytes del socket 'sock'."""
    data = bytearray()
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data.extend(packet)
    return bytes(data)


ocr = PaddleOCR(
    lang="es",
    use_doc_orientation_classify=False,
    use_doc_unwarping=False,
    use_textline_orientation=False
)

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
                response_text = 'ERROR: IMAGEN INVALIDA'
            else:
                result = ocr.predict(input=img_np)
                detected_texts = []
            if result and result[0] is not None:
                ocr_dict_output = result[0].json
                response_data = json.dumps(ocr_dict_output, ensure_ascii=False)
                print("Enviando respuesta JSON válida.")

            else:
                print("No se detectó texto. Enviando respuesta vacía.")
                response_data = json.dumps({})

            response_bytes = response_data.encode('utf-8')

            response_header = struct.pack('>Q', len(response_bytes))

            connection.sendall(response_header)

            connection.sendall(response_bytes)

    except (socket.error, ConnectionResetError) as e:
        print(f"Error de conexión: {e}")
    finally:
        connection.close()
