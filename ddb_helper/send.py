import socket
import sys

HOST = "127.0.0.1"
PORT = 20202
BUFFER_SIZE = 1024
MESSAGE = None

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    print(f"Connecting to {HOST}:{PORT}")
    sock.connect((HOST, PORT))

    while True:
        try:
            user_input = input("Enter an unsigned 64-bit integer: ").strip()
            num = int(user_input)

            if not (0 <= num < 2**64):
                raise ValueError("Number must be a non-negative integer fitting in 64 bits.")

            # Convert the integer to 8 bytes (64 bits) in big-endian order
            # Use 'little' if little-endian is required.
            MESSAGE = num.to_bytes(8, byteorder='big', signed=False)
        except ValueError as e:
            print(f"Invalid input: {e}")
            sys.exit(1)

        try:
            print(f"Sending: {MESSAGE!r}")
            sock.sendall(MESSAGE)

            data = sock.recv(BUFFER_SIZE)
            if data:
                # Assuming the received data is a 4-byte IPv4 address in network byte order (big-endian)
                if len(data) == 4:
                    ip_int = int.from_bytes(data, byteorder='big')
                    ip_str = socket.inet_ntoa(data)
                    print(f"Received raw integer: {ip_int}")
                    print(f"Received IP address: {ip_str}")
                else:
                    print(f"Received unexpected data length ({len(data)} bytes): {data!r}")
            else:
                print("Server closed the connection.")
                break
        except ConnectionRefusedError:
            print(f"Connection refused. Is the server running on {HOST}:{PORT}?")
            sys.exit(1)
        except Exception as e:
            print(f"An error occurred: {e}")
            sys.exit(1)
print("Connection closed.")
