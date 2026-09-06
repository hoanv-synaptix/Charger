using System;
using System.IO.Ports;
using System.Threading;
using System.Collections.Generic;

namespace ChargerDebugApp.Protocol
{
    public class SerialService
    {
        private SerialPort _serialPort;
        private Thread? _readThread;
        private bool _keepReading;
        
        private const byte SOF1 = 0xAA;
        private const byte SOF2 = 0x55;
        private const byte CRC_POLY = 0x07;

        public event Action<byte, byte[]>? OnFrameReceived;
        public event Action<string>? OnLog;
        public event Action<string>? OnError;

        public static SerialService Instance { get; } = new SerialService();
        public DebugProtocolParser Parser { get; } = new DebugProtocolParser();

        private SerialService()
        {
            _serialPort = new SerialPort();
        }

        public string[] GetAvailablePorts()
        {
            return SerialPort.GetPortNames();
        }

        public bool Connect(string portName, int baudRate = 115200)
        {
            try
            {
                if (_serialPort.IsOpen)
                    _serialPort.Close();

                _serialPort.PortName = portName;
                _serialPort.BaudRate = baudRate;
                _serialPort.ReadTimeout = 100;
                _serialPort.Open();

                _keepReading = true;
                _readThread = new Thread(ReadLoop);
                _readThread.IsBackground = true;
                _readThread.Start();

                return true;
            }
            catch (Exception ex)
            {
                OnError?.Invoke($"Connect Error: {ex.Message}");
                return false;
            }
        }

        public void Disconnect()
        {
            _keepReading = false;
            if (_readThread != null && _readThread.IsAlive)
                _readThread.Join(500);

            if (_serialPort.IsOpen)
                _serialPort.Close();
        }

        public bool IsConnected => _serialPort.IsOpen;

        public bool SendFrame(byte cmd, byte[]? payload = null)
        {
            if (!_serialPort.IsOpen) return false;
            
            payload ??= Array.Empty<byte>();
            int len = payload.Length;
            if (len > 255) throw new ArgumentException("Payload too large");

            byte[] frame = new byte[5 + len];
            frame[0] = SOF1;
            frame[1] = SOF2;
            frame[2] = cmd;
            frame[3] = (byte)len;
            
            Array.Copy(payload, 0, frame, 4, len);
            
            byte crc = 0;
            crc = ComputeCrc8(crc, cmd);
            crc = ComputeCrc8(crc, (byte)len);
            for (int i = 0; i < len; i++)
                crc = ComputeCrc8(crc, payload[i]);

            frame[4 + len] = crc;

            try
            {
                _serialPort.Write(frame, 0, frame.Length);
                return true;
            }
            catch (Exception ex)
            {
                OnError?.Invoke($"Write Error: {ex.Message}");
                return false;
            }
        }

        private byte ComputeCrc8(byte initialCrc, byte data)
        {
            int crc = initialCrc ^ data;
            for (int i = 0; i < 8; i++)
            {
                if ((crc & 0x80) != 0)
                    crc = ((crc << 1) ^ CRC_POLY) & 0xFF;
                else
                    crc = (crc << 1) & 0xFF;
            }
            return (byte)crc;
        }

        private void ReadLoop()
        {
            List<byte> buffer = new List<byte>();
            byte[] readBuf = new byte[1024];

            while (_keepReading)
            {
                try
                {
                    if (_serialPort.BytesToRead > 0)
                    {
                        int bytesRead = _serialPort.Read(readBuf, 0, Math.Min(readBuf.Length, _serialPort.BytesToRead));
                        for (int i = 0; i < bytesRead; i++)
                            buffer.Add(readBuf[i]);

                        ProcessBuffer(buffer);
                    }
                    else
                    {
                        Thread.Sleep(10);
                    }
                }
                catch (TimeoutException) { }
                catch (Exception ex)
                {
                    if (_keepReading)
                        OnError?.Invoke($"Read Loop Error: {ex.Message}");
                }
            }
        }

        private void ProcessBuffer(List<byte> buffer)
        {
            while (buffer.Count >= 5)
            {
                // Find SOF
                int sofIndex = -1;
                for (int i = 0; i < buffer.Count - 1; i++)
                {
                    if (buffer[i] == SOF1 && buffer[i + 1] == SOF2)
                    {
                        sofIndex = i;
                        break;
                    }
                }

                if (sofIndex == -1)
                {
                    // No SOF found, keep last byte just in case it's SOF1
                    byte lastByte = buffer[buffer.Count - 1];
                    buffer.Clear();
                    if (lastByte == SOF1) buffer.Add(SOF1);
                    return;
                }

                // Discard garbage before SOF
                if (sofIndex > 0)
                    buffer.RemoveRange(0, sofIndex);

                // Now buffer[0] = SOF1, buffer[1] = SOF2
                if (buffer.Count < 4) return; // Wait for CMD and LEN

                byte cmd = buffer[2];
                byte len = buffer[3];

                int frameLength = 5 + len; // SOF1, SOF2, CMD, LEN, payload..., CRC
                if (buffer.Count < frameLength) return; // Wait for full frame

                // Extract payload
                byte[] payload = new byte[len];
                for (int i = 0; i < len; i++)
                    payload[i] = buffer[4 + i];

                byte receivedCrc = buffer[4 + len];

                // Verify CRC
                byte crc = 0;
                crc = ComputeCrc8(crc, cmd);
                crc = ComputeCrc8(crc, len);
                for (int i = 0; i < len; i++)
                    crc = ComputeCrc8(crc, payload[i]);

                if (crc == receivedCrc)
                {
                    OnFrameReceived?.Invoke(cmd, payload);
                    Parser.ParseFrame(cmd, payload);
                }
                else
                {
                    OnLog?.Invoke($"[WARN] CRC Mismatch. Cmd: {cmd:X2}, Len: {len}, Expected: {crc:X2}, Got: {receivedCrc:X2}");
                }

                // Remove processed frame
                buffer.RemoveRange(0, frameLength);
            }
        }
    }
}