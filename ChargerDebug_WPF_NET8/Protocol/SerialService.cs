using System;
using System.IO.Ports;
using System.Threading;

namespace ChargerDebugApp.Protocol
{
    public class SerialService
    {
        private SerialPort _serialPort;
        private Thread _readThread;
        private bool _keepReading;

        public event Action<byte[]> OnDataReceived;
        public event Action<string> OnError;

        public SerialService()
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
                _serialPort.DataBits = 8;
                _serialPort.Parity = Parity.None;
                _serialPort.StopBits = StopBits.One;
                _serialPort.ReadTimeout = 500;

                _serialPort.Open();
                
                _keepReading = true;
                _readThread = new Thread(ReadPort);
                _readThread.IsBackground = true;
                _readThread.Start();

                return true;
            }
            catch (Exception ex)
            {
                OnError?.Invoke(ex.Message);
                return false;
            }
        }

        public void Disconnect()
        {
            _keepReading = false;
            if (_serialPort.IsOpen)
            {
                try
                {
                    _serialPort.Close();
                }
                catch { }
            }
        }

        private void ReadPort()
        {
            byte[] buffer = new byte[4096];
            while (_keepReading)
            {
                try
                {
                    if (_serialPort.IsOpen && _serialPort.BytesToRead > 0)
                    {
                        int bytesRead = _serialPort.Read(buffer, 0, buffer.Length);
                        if (bytesRead > 0)
                        {
                            byte[] data = new byte[bytesRead];
                            Array.Copy(buffer, data, bytesRead);
                            OnDataReceived?.Invoke(data);
                        }
                    }
                    else
                    {
                        Thread.Sleep(10);
                    }
                }
                catch (TimeoutException) { }
                catch (Exception)
                {
                    // Serial port disconnected or error
                    _keepReading = false;
                }
            }
        }

        public bool IsConnected => _serialPort != null && _serialPort.IsOpen;
    }
}
