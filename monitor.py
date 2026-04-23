import socket
import struct
import time
import sys

# To use this script, you must first compile the proto file:
# protoc --python_out=. instrument.proto
# You may also need to install the protobuf package:
# pip install protobuf

try:
    import instrument_pb2
except ImportError:
    print("Error: instrument_pb2 not found.")
    print("Please run: protoc --python_out=. instrument.proto")
    sys.exit(1)

PICO_IP = '192.168.1.100' # Update this to your Pico's IP
PORT = 1234

def connect_and_monitor():
    print(f"Connecting to {PICO_IP}:{PORT}...")
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((PICO_IP, PORT))
            print("Connected to Pico 2W!")

            # Example: Request a snapshot right away
            cmd = instrument_pb2.Command()
            cmd.request_snapshot = True
            s.sendall(cmd.SerializeToString())

            while True:
                # Receive size prefix or just the message? 
                # Nanopb encoded messages aren't self-delimiting. 
                # However, for simplicity in this demo, we assume one message per recv.
                data = s.recv(2048)
                if not data:
                    print("Connection closed by Pico.")
                    break
                    
                telemetry = instrument_pb2.Telemetry()
                try:
                    telemetry.ParseFromString(data)
                except Exception as e:
                    print(f"Failed to parse telemetry: {e}")
                    continue
                
                print("-" * 40)
                print(f"📊 Stats | Mean: {telemetry.mean:.2f}, StdDev: {telemetry.stddev:.2f}")
                print(f"📈 Hist  | {list(telemetry.histogram)}")
                
                if telemetry.snapshot:
                    # Snapshot is raw 12-bit ADC data (uint16_t)
                    num_samples = len(telemetry.snapshot) // 2
                    raw_data = struct.unpack(f'<{num_samples}H', telemetry.snapshot)
                    print(f"📷 Snapshot Data (First 10): {raw_data[:10]}")
                
                # Periodically request snapshots
                if time.time() % 10 < 1:
                    cmd = instrument_pb2.Command()
                    cmd.request_snapshot = True
                    s.sendall(cmd.SerializeToString())
                    
    except ConnectionRefusedError:
        print(f"Could not connect to {PICO_IP}:{PORT}. Is the Pico running and on the same network?")
    except Exception as e:
        print(f"An error occurred: {e}")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        PICO_IP = sys.argv[1]
    connect_and_monitor()
