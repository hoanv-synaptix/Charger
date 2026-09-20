using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using ChargerDebugApp.Protocol;

namespace ChargerDebugApp.ViewModels
{
    public class ChargeConfigViewModel : INotifyPropertyChanged
    {
        private ChargeCycleConfig _config;
        private ChargeCycleConfig _savedBaseline;

        public event PropertyChangedEventHandler? PropertyChanged;

        public ChargeConfigViewModel()
        {
            _config = ChargeCycleConfig.CreateDefault();
            _savedBaseline = _config.Clone();
        }

        public ChargeCycleConfig Config
        {
            get => _config;
            set
            {
                _config = value;
                _savedBaseline = _config.Clone();
                OnPropertyChanged(string.Empty); // Refresh all bindings
            }
        }

        public int ChargeSourceMode
        {
            get => _config.ChargeSourceMode;
            set
            {
                if (_config.ChargeSourceMode != (byte)value)
                {
                    _config.ChargeSourceMode = (byte)value;
                    OnPropertyChanged(nameof(ChargeSourceMode));
                    OnPropertyChanged(nameof(IsBmsIdEnabled));
                }
            }
        }

        public bool IsBmsIdEnabled => _config.ChargeSourceMode == 0;

        public void LoadConfig(ChargeCycleConfig newConfig)
        {
            _config = newConfig.Clone();
            _savedBaseline = newConfig.Clone();
            OnPropertyChanged(string.Empty);
        }

        public void ResetToDefaults()
        {
            LoadConfig(ChargeCycleConfig.CreateDefault());
        }

        public byte[] GetBytes() => _config.ToBytes();

        public string GetJson() => _config.ToJson();

        public void LoadFromJson(string json)
        {
            var cfg = ChargeCycleConfig.FromJson(json);
            LoadConfig(cfg);
        }

        protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
}