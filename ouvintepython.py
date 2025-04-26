import socket
import threading
import time
from queue import Queue, Empty

# Configurações
DEST_IP = '192.168.100.11'
DEST_PORT = 12345
INTERVALO_ENVIO = 5
TIMEOUT_RECEBIMENTO = 10

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(('', DEST_PORT))
sock.settimeout(1)

# Flags de controle
stop_envio = threading.Event()
mensagem_recebida = threading.Event()
socket_ativo = threading.Event()
socket_ativo.set()

# Fila para mensagens recebidas
fila_mensagens = Queue()

def flood_udp():
    while not stop_envio.is_set() and socket_ativo.is_set():
        try:
            sock.sendto(b'start', (DEST_IP, DEST_PORT))
            print(f"[ENVIADO] start -> {DEST_IP}:{DEST_PORT}")
        except OSError as e:
            print(f"[ERRO ENVIO] {e}")
            break
        time.sleep(INTERVALO_ENVIO)

def ouvir_udp():
    tempo_ultima_msg = None
    while True:
        try:
            data, addr = sock.recvfrom(2048)
            mensagem = data.decode(errors='ignore').strip()
            fila_mensagens.put((addr, mensagem))
            mensagem_recebida.set()
            tempo_ultima_msg = time.time()

        except socket.timeout:
            if mensagem_recebida.is_set() and tempo_ultima_msg:
                if time.time() - tempo_ultima_msg > TIMEOUT_RECEBIMENTO:
                    fila_mensagens.put((None, f"\n[NENHUMA MENSAGEM POR {TIMEOUT_RECEBIMENTO}s]"))
                    break

        except ConnectionResetError as e:
            if e.winerror == 10054:
                continue
            else:
                fila_mensagens.put((None, f"[ERRO RECEBIMENTO] {e}"))
                break

def imprimir_mensagens():
    while socket_ativo.is_set():
        try:
            addr, mensagem = fila_mensagens.get(timeout=1)
            if addr:
                print(f"[RECEBIDO] de {addr[0]}:{addr[1]} -> {mensagem}")
            else:
                print(mensagem)
        except Empty:
            continue

def ciclo_principal():
    thread_impressao = threading.Thread(target=imprimir_mensagens, daemon=True)
    thread_impressao.start()

    while True:
        stop_envio.clear()
        mensagem_recebida.clear()

        thread_envio = threading.Thread(target=flood_udp)
        thread_envio.start()

        ouvir_udp()

        stop_envio.set()
        thread_envio.join()

        opcao = input("\nDeseja reenviar 'start'? (s/n): ").strip().lower()
        if opcao != 's':
            print("\n[FINALIZADO PELO USUÁRIO]")
            break

try:
    ciclo_principal()
finally:
    socket_ativo.clear()
    time.sleep(1)
    sock.close()
