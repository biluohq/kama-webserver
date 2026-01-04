import socket
import time
import sys

def slow_client():
    server_ip = '127.0.0.1'
    server_port = 8080

    try:
        # 1. 连接服务器
        client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        client.connect((server_ip, server_port))
        print(f"Connected to {server_ip}:{server_port}")

        # 2. 发送 "load" 触发大数据逻辑
        client.sendall(b"load")
        print("Sent 'load' command")

        total_received = 0
        expected_size = 100 * 1024 * 1024  # 100 MB

        # 3. 龟速接收数据
        while total_received < expected_size:
            # 每次只读 4KB
            data = client.recv(4096)
            if not data:
                break
            
            total_received += len(data)
            
            # 每收到 1MB 打印一次进度
            if total_received % (1024 * 1024) < 4096:
                sys.stdout.write(f"\rReceived: {total_received / 1024 / 1024:.2f} MB")
                sys.stdout.flush()

            # [关键] 模拟慢速网络：每收一点就睡 0.001 秒
            # 这会导致服务器端的 TCP 发送缓冲区填满 -> 触发应用层 outputBuffer 填满 -> 触发协程挂起
            time.sleep(0.001) 

        print(f"\nDone. Total received: {total_received} bytes")
        client.close()

    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    slow_client()