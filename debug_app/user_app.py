import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import os
import sys
import struct
import time
import json
import queue
from collections import deque
from typing import List, Dict, Optional

from protocol.debug_protocol import (
    DebugProtocolParser, DebugCmd, DebugRsp,
    StdCmd, DRIVER_NAMES, STATE_NAMES, MODULE_TYPE_NAMES, CHARGE_SOURCE_MODE_NAMES,
    ModuleData, BMSData, ChargeCycleConfig, SystemInfo,
    ALARM_FLAG_NAMES, MAXWELL_ALARM_NAMES, LIANMING_ALARM_NAMES,
    TONHE_STATUS_FAULT_NAMES, TONHE_PFC_FAULT_NAMES
)
from services.serial_service import SerialService

def _assets_dir() -> str:
    base = getattr(sys, "_MEIPASS", None) or os.path.dirname(os.path.abspath(__file__))
    return os.path.join(base, "assets")

class ChargerUserApp:
    def __init__(self, root):
        self.root = root
        self.root.title("PKG Battery Charger - User Edition")
        
        self.screen_width = self.root.winfo_screenwidth()
        self.screen_height = self.root.winfo_screenheight()
        
        try:
            native_scale = float(self.root.tk.call("tk", "scaling"))
        except (tk.TclError, ValueError):
            native_scale = 1.0
        screen_scale = self.screen_width / 1366.0
        self.ui_scale = max(0.85, min(1.35, native_scale * screen_scale))
        try:
            self.root.tk.call("tk", "scaling", self.ui_scale)
        except tk.TclError:
            pass

        self.root.geometry("1100x750")
        self.root.minsize(980, 650)
        self._configure_styles()

        # Services
        self.serial = SerialService()
        self.parser = DebugProtocolParser()
        self._frame_queue = queue.Queue()

        # State
        self.charge_config: Optional[ChargeCycleConfig] = None
        self.cfg_vars: Dict[str, tk.StringVar] = {}
        self.cfg_checks: Dict[str, tk.BooleanVar] = {}
        self._latest_frames: Dict[int, bytes] = {}
        self.bms_data: Optional[BMSData] = None
        self.system_info: Optional[SystemInfo] = None
        self.modules: Dict[int, ModuleData] = {}
        
        self.scada_window = None
        self.scada_vars: Dict[str, tk.StringVar] = {}

        self._build_ui()
        
        # Callbacks
        self.serial.on_frame(self._on_frame)
        self._refresh_ports()
        
        # Start GUI Loop
        self.root.after(100, self._gui_update_loop)

    def _configure_styles(self):
        style = ttk.Style()
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        base_font = ("Segoe UI", 10)
        heading_font = ("Segoe UI", 10, "bold")
        style.configure("TLabel", font=base_font)
        style.configure("TButton", font=base_font, padding=4)
        style.configure("TEntry", font=base_font)
        style.configure("TCombobox", font=base_font)
        style.configure("Treeview", font=base_font, rowheight=24)
        style.configure("Treeview.Heading", font=heading_font)

    def _on_frame(self, cmd: int, payload: bytes):
        try:
            self._frame_queue.put_nowait((cmd, payload))
        except queue.Full:
            pass

    def _gui_update_loop(self):
        try:
            loop_start = time.perf_counter()
            deadline = loop_start + 0.008 

            while not self._frame_queue.empty():
                if time.perf_counter() >= deadline:
                    break
                try:
                    cmd, payload = self._frame_queue.get_nowait()
                    self._latest_frames[cmd] = payload 
                except queue.Empty:
                    break

            frames_to_process = list(self._latest_frames.items())
            self._latest_frames.clear()
            
            for cmd, payload in frames_to_process:
                self._process_frame(cmd, payload)
                
            if self.scada_window and self.scada_window.winfo_exists():
                self._update_scada_ui()
                
        except Exception as exc:
            pass
        finally:
            self.root.after(50, self._gui_update_loop)

    def _process_frame(self, cmd: int, payload: bytes):
        result = self.parser.parse_frame(cmd, payload)
        
        if cmd == DebugRsp.GET_CHARGE_CFG and isinstance(result, ChargeCycleConfig):
            self._load_charge_config_to_ui(result)
            messagebox.showinfo("Success", "Configuration loaded from device.")
        elif cmd == DebugRsp.BMS_DATA and isinstance(result, BMSData):
            self.bms_data = result
        elif cmd == DebugRsp.SYSTEM_INFO and isinstance(result, SystemInfo):
            self.system_info = result
        elif cmd == DebugRsp.ALL_MODULES and isinstance(result, list):
            for mod_data in result:
                if isinstance(mod_data, ModuleData):
                    self.modules[mod_data.addr] = mod_data
        elif cmd == DebugRsp.MODULE_DATA and isinstance(result, ModuleData):
            self.modules[result.addr] = result

    # =========================================================================
    # UI BUILD
    # =========================================================================

    def _build_ui(self):
        root = ttk.Frame(self.root)
        root.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)
        root.grid_columnconfigure(0, weight=1)
        root.grid_rowconfigure(1, weight=1)

        self._build_connection_bar(root)

        main_area = ttk.Frame(root)
        main_area.grid(row=1, column=0, sticky="nsew", pady=(10, 0))
        self._build_charge_config_tab(main_area)

    def _build_connection_bar(self, parent):
        frm = ttk.Frame(parent, height=50)
        frm.grid(row=0, column=0, sticky="ew")
        frm.pack_propagate(False)

        ttk.Label(frm, text="Port:").pack(side=tk.LEFT, padx=(10, 0))
        self.cmb_port = ttk.Combobox(frm, width=12, state="readonly")
        self.cmb_port.pack(side=tk.LEFT, padx=(8, 4))

        ttk.Button(frm, text="Refresh", command=self._refresh_ports, width=8).pack(side=tk.LEFT, padx=2)
        self.btn_connect = ttk.Button(frm, text="Connect", command=self._toggle_connect, width=10)
        self.btn_connect.pack(side=tk.LEFT, padx=2)

        sep1 = ttk.Separator(frm, orient="vertical")
        sep1.pack(side=tk.LEFT, fill=tk.Y, padx=8, pady=5)

        self.lbl_status = tk.Label(frm, text="Disconnected", fg="red", font=("Segoe UI", 9, "bold"), bg="#F0F0F0")
        self.lbl_status.pack(side=tk.LEFT, padx=(4, 0))
        
        tk.Frame(frm).pack(side=tk.LEFT, fill=tk.X, expand=True)

        btn_scada = tk.Button(frm, text="Open SCADA Monitor", command=self._open_scada, 
                              bg="#10B981", fg="white", font=("Segoe UI", 10, "bold"), relief="flat")
        btn_scada.pack(side=tk.RIGHT, padx=10, pady=5)


    def _make_scada_var(self, name: str) -> tk.StringVar:
        var = tk.StringVar(value="---")
        self.scada_vars[name] = var
        return var

    def _open_scada(self):
        if self.scada_window is not None and self.scada_window.winfo_exists():
            self.scada_window.lift()
            return
            
        self.scada_window = tk.Toplevel(self.root)
        self.scada_window.title("HMI - Industrial SCADA Monitor")
        self.scada_window.geometry("1280x800")
        self.scada_window.minsize(1024, 768)
        
        # Professional Industrial Dark Theme
        self.bg_color = "#12141A"      # Very dark, almost black blue
        self.panel_color = "#1D212B"   # Dark blue-grey for cards
        self.text_muted = "#7A8294"    # Cool grey for labels
        self.text_primary = "#F0F2F5"  # Off-white for primary text
        self.value_color = "#00E676"   # Neon green for good values
        self.alert_color = "#FF3D00"   # Neon orange/red for alerts
        self.highlight = "#00B0FF"     # Cyan blue for accents
        
        self.scada_window.configure(bg=self.bg_color)
        
        main = tk.Frame(self.scada_window, bg=self.bg_color, padx=20, pady=20)
        main.pack(fill=tk.BOTH, expand=True)
        
        # HEADER
        header = tk.Frame(main, bg=self.bg_color)
        header.pack(fill=tk.X, pady=(0, 20))
        
        title_lbl = tk.Label(header, text="SYSTEM MONITOR", font=("Segoe UI", 22, "bold"), fg=self.highlight, bg=self.bg_color)
        title_lbl.pack(side=tk.LEFT)
        
        status_frame = tk.Frame(header, bg=self.panel_color, padx=20, pady=10, highlightbackground="#2D323E", highlightthickness=1)
        status_frame.pack(side=tk.RIGHT)
        tk.Label(status_frame, text="SYSTEM STATE:", font=("Segoe UI", 12, "bold"), fg=self.text_muted, bg=self.panel_color).pack(side=tk.LEFT, padx=(0, 10))
        lbl_sys_state = tk.Label(status_frame, textvariable=self._make_scada_var("SysState"), font=("Segoe UI", 16, "bold"), fg=self.value_color, bg=self.panel_color)
        lbl_sys_state.pack(side=tk.LEFT)
        
        # MIDDLE DASHBOARD
        dash_frame = tk.Frame(main, bg=self.bg_color)
        dash_frame.pack(fill=tk.BOTH, expand=True, pady=(0, 20))
        dash_frame.grid_columnconfigure(0, weight=1)
        dash_frame.grid_columnconfigure(1, weight=1)
        dash_frame.grid_columnconfigure(2, weight=1)
        dash_frame.grid_rowconfigure(0, weight=1)
        
        def create_card(parent, title, col, row=0):
            card = tk.Frame(parent, bg=self.panel_color, highlightbackground="#2D323E", highlightthickness=1)
            card.grid(row=row, column=col, sticky="nsew", padx=10)
            tk.Label(card, text=title, font=("Segoe UI", 12, "bold"), fg=self.text_primary, bg=self.panel_color, anchor="w").pack(fill=tk.X, padx=20, pady=15)
            tk.Frame(card, bg="#2D323E", height=1).pack(fill=tk.X)
            content_frame = tk.Frame(card, bg=self.panel_color, padx=20, pady=20)
            content_frame.pack(fill=tk.BOTH, expand=True)
            return content_frame

        def add_metric_block(parent, row, col, label, var_name, unit, color=self.text_primary):
            blk = tk.Frame(parent, bg=self.panel_color)
            blk.grid(row=row, column=col, sticky="nsew", padx=10, pady=15)
            tk.Label(blk, text=label.upper(), font=("Segoe UI", 9, "bold"), fg=self.text_muted, bg=self.panel_color, anchor="w").pack(anchor="w")
            
            val_frame = tk.Frame(blk, bg=self.panel_color)
            val_frame.pack(anchor="w", pady=(5, 0))
            
            tk.Label(val_frame, textvariable=self._make_scada_var(var_name), font=("Segoe UI", 24, "bold"), fg=color, bg=self.panel_color).pack(side=tk.LEFT)
            if unit:
                tk.Label(val_frame, text=unit, font=("Segoe UI", 12, "bold"), fg=self.text_muted, bg=self.panel_color).pack(side=tk.LEFT, padx=(5, 0))
            return blk

        # 1. CHARGER PANEL
        chr_card = create_card(dash_frame, "CHARGER CONTROLLER", 0)
        chr_card.grid_columnconfigure(0, weight=1)
        chr_card.grid_columnconfigure(1, weight=1)
        
        add_metric_block(chr_card, 0, 0, "Output Voltage", "ChrVolt", "V", self.highlight)
        add_metric_block(chr_card, 0, 1, "Output Current", "ChrCurr", "A", self.highlight)
        add_metric_block(chr_card, 1, 0, "Output Power", "ChrPower", "kW")
        add_metric_block(chr_card, 1, 1, "Max Temp", "ChrTemp", "°C")
        add_metric_block(chr_card, 2, 0, "Modules Online", "ChrMods", "")
        add_metric_block(chr_card, 2, 1, "Control Mode", "ChrMode", "")
        
        # 2. POWER FLOW PANEL
        flow_card = create_card(dash_frame, "POWER FLOW", 1)
        
        self.flow_cvs = tk.Canvas(flow_card, bg=self.panel_color, highlightthickness=0, height=220)
        self.flow_cvs.pack(fill=tk.BOTH, expand=True)
        
        # Draw Power Flow Arrow from Charger to Battery
        self.flow_cvs.create_line(30, 110, 100, 110, arrow=tk.LAST, fill=self.value_color, width=4, dash=(10, 5))
        
        # Draw a beautiful battery
        # cw=320, ch=220, bx=120, by=40, bw=80, bh=140
        bx, by, bw, bh = 120, 40, 80, 140
        
        # Tip
        self.flow_cvs.create_rectangle(bx + 25, by - 10, bx + 55, by, fill=self.text_muted, outline="")
        # Border
        self.flow_cvs.create_rectangle(bx, by, bx + bw, by + bh, outline=self.text_muted, width=4)
        
        # Inner Fill (will be updated dynamically)
        self.soc_rect = self.flow_cvs.create_rectangle(bx + 5, by + bh - 5, bx + bw - 5, by + bh - 5, fill=self.value_color, outline="")
        
        # Percent text centered
        self.lbl_soc_pct = self.flow_cvs.create_text(bx + bw/2, by + bh/2, text="0%", font=("Segoe UI", 26, "bold"), fill=self.text_primary)
        
        # Target metrics at bottom of center
        tgt_frame = tk.Frame(flow_card, bg=self.panel_color)
        tgt_frame.pack(fill=tk.X, pady=(10, 0))
        tgt_frame.grid_columnconfigure(0, weight=1)
        tgt_frame.grid_columnconfigure(1, weight=1)
        
        add_metric_block(tgt_frame, 0, 0, "Target Voltage", "TgtVolt", "V", "#FFCA28")
        add_metric_block(tgt_frame, 0, 1, "Target Current", "TgtCurr", "A", "#FFCA28")
        
        # 3. BATTERY (BMS) PANEL
        bat_card = create_card(dash_frame, "BATTERY STATUS (BMS)", 2)
        bat_card.grid_columnconfigure(0, weight=1)
        bat_card.grid_columnconfigure(1, weight=1)
        
        add_metric_block(bat_card, 0, 0, "Pack Voltage", "BatVolt", "V", self.value_color)
        add_metric_block(bat_card, 0, 1, "Pack Current", "BatCurr", "A", self.value_color)
        add_metric_block(bat_card, 1, 0, "State of Health", "BatSOH", "%")
        add_metric_block(bat_card, 1, 1, "Max/Min Cell", "BatCellV", "V")
        add_metric_block(bat_card, 2, 0, "Max Temp", "BatMaxT", "°C")
        add_metric_block(bat_card, 2, 1, "Min Temp", "BatMinT", "°C")
        
        # BOTTOM AREA (Split into Modules Table and Alarms)
        bottom_frame = tk.Frame(main, bg=self.bg_color)
        bottom_frame.pack(fill=tk.BOTH, expand=True)
        bottom_frame.grid_columnconfigure(0, weight=3)
        bottom_frame.grid_columnconfigure(1, weight=1)
        
        # Modules Table
        mod_card = create_card(bottom_frame, "INDIVIDUAL MODULE DATA", 0)
        
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("Modern.Treeview", background=self.panel_color, foreground=self.text_primary, fieldbackground=self.panel_color, borderwidth=0, rowheight=35, font=("Segoe UI", 11))
        style.configure("Modern.Treeview.Heading", background="#2D323E", foreground=self.text_muted, font=("Segoe UI", 10, "bold"), borderwidth=0)
        style.map("Modern.Treeview", background=[('selected', self.highlight)], foreground=[('selected', '#FFFFFF')])
        
        columns = ("addr", "state", "vac", "vdc", "idc", "pwr", "temp")
        self.tree_modules = ttk.Treeview(mod_card, columns=columns, show="headings", style="Modern.Treeview", height=5)
        self.tree_modules.heading("addr", text="ADDR")
        self.tree_modules.heading("state", text="STATE")
        self.tree_modules.heading("vac", text="AC IN (V)")
        self.tree_modules.heading("vdc", text="DC OUT (V)")
        self.tree_modules.heading("idc", text="DC OUT (A)")
        self.tree_modules.heading("pwr", text="POWER (kW)")
        self.tree_modules.heading("temp", text="TEMP (°C)")
        
        for col in columns:
            self.tree_modules.column(col, width=80, anchor="center")
        self.tree_modules.pack(fill=tk.BOTH, expand=True)
        
        # Alarms Log
        alrm_card = create_card(bottom_frame, "SYSTEM ALARMS", 1)
        self.scada_alarms_list = tk.Listbox(alrm_card, font=("Segoe UI", 11), bg=self.panel_color, fg=self.alert_color, bd=0, highlightthickness=0, selectbackground=self.panel_color)
        self.scada_alarms_list.pack(fill=tk.BOTH, expand=True)

    def _update_scada_ui(self):
        if not self.scada_window or not self.scada_window.winfo_exists():
            return
            
        # 1. System & Charger Info
        if self.system_info:
            info = self.system_info
            
            # Sys State
            state_str = info.get_controller_state_name() if hasattr(info, 'get_controller_state_name') else str(info.controller_state)
            chg_status = info.get_charge_status_name() if hasattr(info, 'get_charge_status_name') else "Unknown"
            self.scada_vars["SysState"].set(f"{state_str} | {chg_status}")
            
            # Charger metrics
            self.scada_vars["ChrVolt"].set(f"{getattr(info, 'total_voltage', 0.0):.1f}")
            self.scada_vars["ChrCurr"].set(f"{getattr(info, 'total_current', 0.0):.1f}")
            pwr = (getattr(info, 'total_voltage', 0.0) * getattr(info, 'total_current', 0.0)) / 1000.0
            self.scada_vars["ChrPower"].set(f"{pwr:.1f}")
            self.scada_vars["ChrTemp"].set(f"{getattr(info, 'max_temp_dcdc', 0.0):.1f}")
            self.scada_vars["ChrMods"].set(f"{getattr(info, 'modules_online', 0)}/{getattr(info, 'modules_total', 0)}")
            self.scada_vars["ChrMode"].set(info.get_charge_source_mode_name() if hasattr(info, 'get_charge_source_mode_name') else str(info.charge_source_mode))
            
            self.scada_vars["TgtVolt"].set(f"{getattr(info, 'controller_target_voltage', 0.0):.1f}")
            self.scada_vars["TgtCurr"].set(f"{getattr(info, 'controller_target_current_total', 0.0):.1f}")

        # 2. Battery & Alarms
        alarms = []
        if self.bms_data:
            bms = self.bms_data
            self.scada_vars["BatVolt"].set(f"{bms.batt_voltage:.1f}")
            self.scada_vars["BatCurr"].set(f"{bms.batt_current:.1f}")
            self.scada_vars["BatSOH"].set(f"{bms.soh:.0f}")
            self.scada_vars["BatCellV"].set(f"{bms.max_cell_volt:.2f}/{bms.min_cell_volt:.2f}")
            self.scada_vars["BatMaxT"].set(f"{bms.max_cell_temp:.1f}")
            self.scada_vars["BatMinT"].set(f"{bms.min_cell_temp:.1f}")
            
            soc = bms.soc
            if hasattr(self, 'flow_cvs') and hasattr(self, 'lbl_soc_pct'):
                self.flow_cvs.itemconfig(self.lbl_soc_pct, text=f"{soc:.0f}%")
                
                # Animate battery rect
                # cw=320, ch=220, bx=120, by=40, bw=80, bh=140
                bx, by, bw, bh = 120, 40, 80, 140
                y_bottom = by + bh - 5
                y_top_max = by + 5
                h_max = y_bottom - y_top_max
                h = h_max * (soc / 100.0)
                y_top = y_bottom - h
                
                self.flow_cvs.coords(self.soc_rect, bx + 5, y_top, bx + bw - 5, y_bottom)
                
                if soc < 20:
                    self.flow_cvs.itemconfig(self.soc_rect, fill=self.alert_color)
                elif soc < 50:
                    self.flow_cvs.itemconfig(self.soc_rect, fill="#FFCA28")
                else:
                    self.flow_cvs.itemconfig(self.soc_rect, fill=self.value_color)

            
            flags = bms.alarm_flags
            if flags:
                for bit, name in ALARM_FLAG_NAMES.items():
                    if flags & (1 << bit):
                        alarms.append(f"BMS: {name}")
        
        # 3. Modules Detail
        if hasattr(self, 'tree_modules') and self.tree_modules.winfo_exists():
            for item in self.tree_modules.get_children():
                self.tree_modules.delete(item)
                
            for addr, mod in sorted(self.modules.items()):
                state_str = STATE_NAMES.get(mod.state, f"St {mod.state}") if isinstance(STATE_NAMES, dict) else f"St {mod.state}"
                v_ac = f"{mod.v_ac_a:.1f}" if getattr(mod, 'v_ac_a', None) is not None else "---"
                v_dc = f"{mod.voltage:.1f}" if mod.voltage is not None else "---"
                i_dc = f"{mod.current:.1f}" if mod.current is not None else "---"
                p_kw = f"{(mod.voltage * mod.current)/1000.0:.1f}" if mod.voltage and mod.current else "---"
                t_dc = f"{mod.temp_dcdc:.1f}" if getattr(mod, 'temp_dcdc', None) is not None else "---"
                
                self.tree_modules.insert("", "end", values=(f"0x{addr:02X}", state_str, v_ac, v_dc, i_dc, p_kw, t_dc))
                
                # Check module alarms
                if mod.driver == 1 and mod.status_flags:
                    for bit, name in MAXWELL_ALARM_NAMES.items():
                        if mod.status_flags & (1 << bit):
                            alarms.append(f"M{addr:02X}: {name}")
                elif mod.driver == 2 and (mod.status_flags & 0xFFFF):
                    for bit, name in LIANMING_ALARM_NAMES.items():
                        if mod.status_flags & (1 << bit):
                            alarms.append(f"M{addr:02X}: {name}")
                elif mod.driver == 3 and (mod.status_flags & 0xFFFF):
                    for bit, name in TONHE_STATUS_FAULT_NAMES.items():
                        if mod.status_flags & (1 << bit):
                            alarms.append(f"M{addr:02X}: {name}")
                            
        # Alarms List
        if hasattr(self, 'scada_alarms_list') and self.scada_alarms_list.winfo_exists():
            self.scada_alarms_list.delete(0, tk.END)
            if not alarms:
                self.scada_alarms_list.insert(tk.END, "✓ SYSTEM NORMAL")
                self.scada_alarms_list.config(fg=self.value_color)
            else:
                self.scada_alarms_list.config(fg=self.alert_color)
                for a in set(alarms):
                    self.scada_alarms_list.insert(tk.END, f"⚠ {a}")

    def _refresh_ports(self):
        from serial.tools.list_ports import comports
        ports = [p.device for p in comports()]
        self.cmb_port["values"] = ports
        if ports:
            if not self.cmb_port.get() or self.cmb_port.get() not in ports:
                self.cmb_port.current(0)
        else:
            self.cmb_port.set("")

    def _toggle_connect(self):
        if self.serial.is_connected():
            self.serial.disconnect()
            self.btn_connect.config(text="Connect")
            self.lbl_status.config(text="Disconnected", fg="red")
        else:
            port = self.cmb_port.get()
            if not port:
                messagebox.showerror("Error", "No port selected")
                return
            if self.serial.connect(port, 115200):
                self.btn_connect.config(text="Disconnect")
                self.lbl_status.config(text="Connected", fg="green")
            else:
                messagebox.showerror("Error", f"Failed to connect to {port}")

    # =========================================================================
    # CONFIGURATION UI
    # =========================================================================

    def _build_charge_config_tab(self, parent):
        parent.grid_columnconfigure(0, weight=1)
        parent.grid_rowconfigure(0, weight=1)
        
        wrapper = tk.Frame(parent, bg="#F0F0F0")
        wrapper.grid(row=0, column=0, sticky="nsew")
        wrapper.grid_columnconfigure(0, weight=1)
        wrapper.grid_rowconfigure(0, weight=1)

        canvas = tk.Canvas(wrapper, bg="#F0F0F0", highlightthickness=0)
        canvas.grid(row=0, column=0, sticky="nsew")
        v_scroll = ttk.Scrollbar(wrapper, orient="vertical", command=canvas.yview)
        v_scroll.grid(row=0, column=1, sticky="ns")
        canvas.configure(yscrollcommand=v_scroll.set)

        body = tk.Frame(canvas, bg="#F0F0F0")
        body_window = canvas.create_window((0, 0), window=body, anchor="nw")

        def sync_scrollregion(_event=None):
            canvas.configure(scrollregion=canvas.bbox("all"))

        body.bind("<Configure>", sync_scrollregion)
        
        def sync_width(event):
            canvas.itemconfigure(body_window, width=event.width)
        canvas.bind("<Configure>", sync_width)

        def _on_mousewheel(event):
            canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
            return "break"
            
        canvas.bind_all("<MouseWheel>", _on_mousewheel)

        body.grid_columnconfigure(0, weight=1)
        body.grid_columnconfigure(1, weight=1)
        
        # Toolbar
        toolbar = tk.Frame(body, bg="#E9EEF5", bd=1, relief="solid")
        toolbar.grid(row=0, column=0, columnspan=2, sticky="ew", padx=10, pady=(10, 10))
        
        ttk.Button(toolbar, text="Read from Device", command=self._request_charge_config).pack(side=tk.LEFT, padx=5, pady=5)
        ttk.Button(toolbar, text="Write to Device", command=self._send_charge_config).pack(side=tk.LEFT, padx=5, pady=5)
        
        tk.Frame(toolbar, width=2, bg="#CCC").pack(side=tk.LEFT, fill=tk.Y, padx=10, pady=5)
        
        ttk.Button(toolbar, text="Load File", command=self._import_charge_config).pack(side=tk.LEFT, padx=5, pady=5)
        ttk.Button(toolbar, text="Save File", command=self._export_charge_config).pack(side=tk.LEFT, padx=5, pady=5)

        tk.Frame(toolbar, width=2, bg="#CCC").pack(side=tk.LEFT, fill=tk.Y, padx=10, pady=5)
        
        # Start/Stop
        btn_start = tk.Button(toolbar, text="▶ START CHARGING", command=self._start, bg="#DFF5E3", fg="#1B5E20", font=("Segoe UI", 10, "bold"), cursor="hand2")
        btn_start.pack(side=tk.RIGHT, padx=5, pady=5)
        btn_stop = tk.Button(toolbar, text="⏹ STOP CHARGING", command=self._stop, bg="#FFF4CC", fg="#F57F17", font=("Segoe UI", 10, "bold"), cursor="hand2")
        btn_stop.pack(side=tk.RIGHT, padx=5, pady=5)

        self._build_charge_general_group(body)
        self._build_charge_control_group(body)
        self._build_charge_protection_group(body)
        
        self._load_charge_config_defaults()

    def _make_cfg_var(self, key: str, default: str = "0") -> tk.StringVar:
        if key not in self.cfg_vars:
            self.cfg_vars[key] = tk.StringVar(value=default)
        return self.cfg_vars[key]

    def _make_cfg_check(self, key: str, default: bool = False) -> tk.BooleanVar:
        if key not in self.cfg_checks:
            self.cfg_checks[key] = tk.BooleanVar(value=default)
        return self.cfg_checks[key]

    def _add_cfg_entry(self, parent, row: int, column: int, label: str, key: str, unit: str = "", width: int = 10):
        frame = tk.Frame(parent, bg="#F0F0F0")
        frame.grid(row=row, column=column, sticky="ew", padx=6, pady=4)
        frame.grid_columnconfigure(1, weight=1)
        tk.Label(frame, text=label, font=("Segoe UI", 9), bg="#F0F0F0", anchor="w").grid(row=0, column=0, sticky="w")
        entry = ttk.Entry(frame, textvariable=self._make_cfg_var(key), width=width)
        entry.grid(row=0, column=1, sticky="ew", padx=(8, 0))
        if key == "can_battery_id":
            self.cfg_bms_can_id_entry = entry
        elif key == "source_module_count":
            self.cfg_source_module_entry = entry

    def _add_cfg_check(self, parent, row: int, column: int, label: str, key: str):
        frame = tk.Frame(parent, bg="#F0F0F0")
        frame.grid(row=row, column=column, sticky="w", padx=6, pady=4)
        tk.Label(frame, text=label, font=("Segoe UI", 9), bg="#F0F0F0").pack(side=tk.LEFT)
        tk.Checkbutton(frame, variable=self._make_cfg_check(key), bg="#F0F0F0", activebackground="#F0F0F0").pack(side=tk.LEFT, padx=(8, 0))

    def _build_stage_panel(self, parent, title: str, enabled_key: str, delta_key: str,
                           threshold_keys: list[str], current_keys: list[str],
                           threshold_unit: str, current_unit: str,
                           threshold_label_prefix: str, current_label_prefix: str = "Current"):
        panel = tk.LabelFrame(parent, text=title, font=("Segoe UI", 8, "bold"), padx=10, pady=8, bg="#F0F0F0")
        panel.pack(fill=tk.X, pady=(0, 10))
        top = tk.Frame(panel, bg="#F0F0F0")
        top.pack(fill=tk.X, pady=(0, 8))
        top.grid_columnconfigure(2, weight=1)
        self._add_cfg_check(top, 0, 0, "Enabled", enabled_key)
        self._add_cfg_entry(top, 0, 1, "Delta", delta_key, threshold_unit, width=8)

        grid = tk.Frame(panel, bg="#F0F0F0")
        grid.pack(fill=tk.X)
        for col in range(6):
            grid.grid_columnconfigure(col, weight=1, uniform=f"{title}_grid")

        tk.Label(grid, text="", bg="#F0F0F0").grid(row=0, column=0, padx=4, pady=(0, 6))
        for idx in range(5):
            tk.Label(grid, text=f"{threshold_label_prefix} {idx + 1}", font=("Segoe UI", 8, "bold"), bg="#F0F0F0", fg="#355C7D").grid(row=0, column=idx + 1, padx=4, pady=(0, 6))

        tk.Label(grid, text="Thresholds", font=("Segoe UI", 8), bg="#F0F0F0", anchor="w").grid(row=1, column=0, sticky="w", padx=4, pady=4)
        for idx, key in enumerate(threshold_keys):
            ttk.Entry(grid, textvariable=self._make_cfg_var(key), width=9).grid(row=1, column=idx + 1, padx=4, pady=4, sticky="ew")

        tk.Label(grid, text="", bg="#F0F0F0").grid(row=2, column=0, padx=4, pady=(8, 4))
        for idx, key in enumerate(current_keys):
            tk.Label(grid, text=f"{current_label_prefix} {idx + 1}-{idx + 2}", font=("Segoe UI", 8, "bold"), bg="#F0F0F0", fg="#6C5B7B").grid(row=2, column=idx + 1, padx=4, pady=(8, 4))
            
        tk.Label(grid, text="Current Limits", font=("Segoe UI", 8), bg="#F0F0F0", anchor="w").grid(row=3, column=0, sticky="w", padx=4, pady=4)
        for idx, key in enumerate(current_keys):
            ttk.Entry(grid, textvariable=self._make_cfg_var(key), width=9).grid(row=3, column=idx + 1, padx=4, pady=4, sticky="ew")

    def _build_charge_general_group(self, parent):
        group = tk.LabelFrame(parent, text="General Limits", font=("Segoe UI", 9, "bold"), padx=10, pady=8, bg="#F0F0F0")
        group.grid(row=1, column=0, sticky="nsew", padx=10, pady=10)
        group.grid_columnconfigure(0, weight=1)
        group.grid_columnconfigure(1, weight=1)

        left = tk.LabelFrame(group, text="Pack Parameters", font=("Segoe UI", 8, "bold"), padx=8, pady=8, bg="#F0F0F0")
        left.grid(row=1, column=0, sticky="nsew", padx=(4, 6))
        left.grid_columnconfigure(0, weight=1)
        left.grid_columnconfigure(1, weight=1)
        self._add_cfg_entry(left, 0, 0, "Battery Capacity", "battery_capacity_ah", "Ah")
        self._add_cfg_entry(left, 0, 1, "Temperature Limit", "temp_limit_c", "C")
        self._add_cfg_entry(left, 1, 0, "V Min", "vmin_v", "V")
        self._add_cfg_entry(left, 1, 1, "V Max", "vmax_v", "V")
        self._add_cfg_entry(left, 2, 0, "I Min", "imin_c", "C")
        self._add_cfg_entry(left, 2, 1, "I Max", "imax_c", "C")

        right = tk.LabelFrame(group, text="Charge Window", font=("Segoe UI", 8, "bold"), padx=8, pady=8, bg="#F0F0F0")
        right.grid(row=1, column=1, sticky="nsew", padx=(6, 4))
        right.grid_columnconfigure(0, weight=1)
        right.grid_columnconfigure(1, weight=1)
        self._add_cfg_entry(right, 0, 0, "V Precharge", "vpre_v", "V")
        self._add_cfg_entry(right, 0, 1, "V Low", "vlow_v", "V")
        self._add_cfg_entry(right, 1, 0, "I Precharge", "ipre_c", "C")
        self._add_cfg_entry(right, 1, 1, "I Low", "ilow_c", "C")

    def _build_charge_control_group(self, parent):
        group = tk.LabelFrame(parent, text="Charging Strategy", font=("Segoe UI", 9, "bold"), padx=10, pady=8, bg="#F0F0F0")
        group.grid(row=2, column=0, sticky="nsew", padx=10, pady=10)

        self._build_stage_panel(
            group, "Cell Voltage Stages", "cell_volt_enabled", "cell_volt_delta_v",
            ["cell_volt_1_v", "cell_volt_2_v", "cell_volt_3_v", "cell_volt_4_v", "cell_volt_5_v"],
            ["cell_curr_1_c", "cell_curr_2_c", "cell_curr_3_c", "cell_curr_4_c"],
            "V", "C", "Cell Voltage", "Current"
        )
        self._build_stage_panel(
            group, "Temperature Stages", "temp_enabled", "temp_delta_c",
            ["temp_1_c", "temp_2_c", "temp_3_c", "temp_4_c", "temp_5_c"],
            ["temp_curr_1_c", "temp_curr_2_c", "temp_curr_3_c", "temp_curr_4_c"],
            "C", "C", "Temperature", "Current"
        )
        self._build_stage_panel(
            group, "SOC Stages", "soc_enabled", "soc_delta_pct",
            ["soc_1_pct", "soc_2_pct", "soc_3_pct", "soc_4_pct", "soc_5_pct"],
            ["soc_curr_1_c", "soc_curr_2_c", "soc_curr_3_c", "soc_curr_4_c"],
            "%", "C", "SOC", "Current"
        )

    def _build_charge_protection_group(self, parent):
        group = tk.LabelFrame(parent, text="Protection and Hardware", font=("Segoe UI", 9, "bold"), padx=10, pady=8, bg="#F0F0F0")
        group.grid(row=1, column=1, rowspan=2, sticky="nsew", padx=10, pady=10)
        group.grid_columnconfigure(0, weight=1)

        system = tk.LabelFrame(group, text="System Mapping", font=("Segoe UI", 8, "bold"), padx=8, pady=8, bg="#F0F0F0")
        system.grid(row=1, column=0, sticky="ew", pady=(0, 10))
        system.grid_columnconfigure(0, weight=1)
        system.grid_columnconfigure(1, weight=1)
        self._add_cfg_entry(system, 0, 0, "BMS CAN ID", "can_battery_id")

        source_row = tk.Frame(system, bg="#F0F0F0")
        source_row.grid(row=0, column=1, sticky="ew", padx=6, pady=4)
        tk.Label(source_row, text="Charge Source", font=("Segoe UI", 9), bg="#F0F0F0").pack(side=tk.LEFT)
        self.cfg_charge_source_mode = ttk.Combobox(
            source_row, state="readonly", width=24,
            values=[CHARGE_SOURCE_MODE_NAMES[idx] for idx in sorted(CHARGE_SOURCE_MODE_NAMES)]
        )
        self.cfg_charge_source_mode.pack(side=tk.LEFT, padx=(8, 0))
        self.cfg_charge_source_mode.current(0)
        
        self.cfg_module_type = ttk.Combobox(source_row, state="readonly", width=12, values=list(MODULE_TYPE_NAMES.values()))
        self.cfg_module_type.pack(side=tk.LEFT, padx=(8, 0))
        self.cfg_module_type.current(0)
        
        self._add_cfg_entry(system, 1, 0, "Module Count", "source_module_count")

        jack_charge = tk.LabelFrame(group, text="Charge Jack Protection", font=("Segoe UI", 8, "bold"), padx=8, pady=8, bg="#F0F0F0")
        jack_charge.grid(row=2, column=0, sticky="ew", pady=(0, 10))
        jack_charge.grid_columnconfigure(0, weight=1)
        jack_charge.grid_columnconfigure(1, weight=1)
        jack_charge.grid_columnconfigure(2, weight=1)
        self._add_cfg_check(jack_charge, 0, 0, "Enabled", "protect_jack_charge_enabled")
        self._add_cfg_entry(jack_charge, 0, 1, "Delta V", "protect_jack_charge_delta_v", "V")
        self._add_cfg_entry(jack_charge, 0, 2, "Delay", "protect_jack_charge_delay_s", "s")

        jack_temp = tk.LabelFrame(group, text="Jack Temperature Protection", font=("Segoe UI", 8, "bold"), padx=8, pady=8, bg="#F0F0F0")
        jack_temp.grid(row=3, column=0, sticky="ew", pady=(0, 10))
        jack_temp.grid_columnconfigure(0, weight=1)
        jack_temp.grid_columnconfigure(1, weight=1)
        self._add_cfg_check(jack_temp, 0, 0, "Enabled", "protect_jack_temp_enabled")
        self._add_cfg_entry(jack_temp, 0, 1, "Delay", "protect_jack_temp_delay_s", "s")
        self._add_cfg_entry(jack_temp, 1, 0, "Threshold", "protect_jack_temp_threshold_c", "C")
        self._add_cfg_entry(jack_temp, 1, 1, "Delta", "protect_jack_temp_delta_c", "C")
        self._add_cfg_entry(jack_temp, 2, 0, "Power Limit", "protect_jack_temp_power_limit_pct", "%")
        self._add_cfg_entry(jack_temp, 2, 1, "Trip Temp", "protect_jack_temp_trip_c", "C")
        
        hw_limits = tk.LabelFrame(group, text="Hardware Limits", font=("Segoe UI", 8, "bold"), padx=8, pady=8, bg="#F0F0F0")
        hw_limits.grid(row=4, column=0, sticky="ew")
        hw_limits.grid_columnconfigure(0, weight=1)
        hw_limits.grid_columnconfigure(1, weight=1)
        self._add_cfg_entry(hw_limits, 0, 0, "Module U Min", "module_u_min_v", "V")
        self._add_cfg_entry(hw_limits, 0, 1, "Module U Max", "module_u_max_v", "V")
        self._add_cfg_entry(hw_limits, 1, 0, "Module I Min", "module_i_min_a", "A")
        self._add_cfg_entry(hw_limits, 1, 1, "Module I Max", "module_i_max_a", "A")


    def _load_charge_config_defaults(self):
        try:
            with open("charge_config.json", "r") as f:
                data = json.load(f)
                cfg = ChargeCycleConfig(**data)
                self._load_charge_config_to_ui(cfg)
        except Exception:
            pass

    def _import_charge_config(self):
        file_path = filedialog.askopenfilename(defaultextension=".json", filetypes=[("JSON files", "*.json")])
        if file_path:
            try:
                with open(file_path, "r") as f:
                    data = json.load(f)
                    cfg = ChargeCycleConfig(**data)
                    self._load_charge_config_to_ui(cfg)
            except Exception as e:
                messagebox.showerror("Error", f"Failed to load config: {e}")

    def _export_charge_config(self):
        file_path = filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("JSON files", "*.json")])
        if file_path:
            try:
                cfg = self._collect_charge_config_from_ui()
                with open(file_path, "w") as f:
                    import dataclasses
                    json.dump(dataclasses.asdict(cfg), f, indent=4)
                messagebox.showinfo("Success", "Configuration saved.")
            except Exception as e:
                messagebox.showerror("Error", f"Failed to save config: {e}")

    def _load_charge_config_to_ui(self, cfg: ChargeCycleConfig):
        self.charge_config = cfg
        self.cfg_vars["battery_capacity_ah"].set(f"{cfg.battery_capacity_ah:.2f}")
        self.cfg_vars["imin_c"].set(f"{cfg.imin_c:.2f}")
        self.cfg_vars["imax_c"].set(f"{cfg.imax_c:.2f}")
        self.cfg_vars["ipre_c"].set(f"{cfg.ipre_c:.2f}")
        self.cfg_vars["ilow_c"].set(f"{cfg.ilow_c:.2f}")
        self.cfg_vars["vmin_v"].set(f"{cfg.vmin_v:.2f}")
        self.cfg_vars["vmax_v"].set(f"{cfg.vmax_v:.2f}")
        self.cfg_vars["vpre_v"].set(f"{cfg.vpre_v:.2f}")
        self.cfg_vars["vlow_v"].set(f"{cfg.vlow_v:.2f}")
        self.cfg_vars["temp_limit_c"].set(f"{cfg.temp_limit_c:.2f}")
        self.cfg_checks["cell_volt_enabled"].set(cfg.cell_volt_enabled)
        self.cfg_vars["cell_volt_delta_v"].set(f"{cfg.cell_volt_delta_v:.3f}")
        self.cfg_vars["cell_volt_1_v"].set(f"{cfg.cell_volt_1_v:.3f}")
        self.cfg_vars["cell_volt_2_v"].set(f"{cfg.cell_volt_2_v:.3f}")
        self.cfg_vars["cell_volt_3_v"].set(f"{cfg.cell_volt_3_v:.3f}")
        self.cfg_vars["cell_volt_4_v"].set(f"{cfg.cell_volt_4_v:.3f}")
        self.cfg_vars["cell_volt_5_v"].set(f"{cfg.cell_volt_5_v:.3f}")
        self.cfg_vars["cell_curr_1_c"].set(f"{cfg.cell_curr_1_c:.2f}")
        self.cfg_vars["cell_curr_2_c"].set(f"{cfg.cell_curr_2_c:.2f}")
        self.cfg_vars["cell_curr_3_c"].set(f"{cfg.cell_curr_3_c:.2f}")
        self.cfg_vars["cell_curr_4_c"].set(f"{cfg.cell_curr_4_c:.2f}")
        self.cfg_checks["temp_enabled"].set(cfg.temp_enabled)
        self.cfg_vars["temp_delta_c"].set(f"{cfg.temp_delta_c:.2f}")
        self.cfg_vars["temp_1_c"].set(f"{cfg.temp_1_c:.2f}")
        self.cfg_vars["temp_2_c"].set(f"{cfg.temp_2_c:.2f}")
        self.cfg_vars["temp_3_c"].set(f"{cfg.temp_3_c:.2f}")
        self.cfg_vars["temp_4_c"].set(f"{cfg.temp_4_c:.2f}")
        self.cfg_vars["temp_5_c"].set(f"{cfg.temp_5_c:.2f}")
        self.cfg_vars["temp_curr_1_c"].set(f"{cfg.temp_curr_1_c:.2f}")
        self.cfg_vars["temp_curr_2_c"].set(f"{cfg.temp_curr_2_c:.2f}")
        self.cfg_vars["temp_curr_3_c"].set(f"{cfg.temp_curr_3_c:.2f}")
        self.cfg_vars["temp_curr_4_c"].set(f"{cfg.temp_curr_4_c:.2f}")
        self.cfg_checks["soc_enabled"].set(cfg.soc_enabled)
        self.cfg_vars["soc_delta_pct"].set(f"{cfg.soc_delta_pct:.2f}")
        self.cfg_vars["soc_1_pct"].set(f"{cfg.soc_1_pct:.2f}")
        self.cfg_vars["soc_2_pct"].set(f"{cfg.soc_2_pct:.2f}")
        self.cfg_vars["soc_3_pct"].set(f"{cfg.soc_3_pct:.2f}")
        self.cfg_vars["soc_4_pct"].set(f"{cfg.soc_4_pct:.2f}")
        self.cfg_vars["soc_5_pct"].set(f"{cfg.soc_5_pct:.2f}")
        self.cfg_vars["soc_curr_1_c"].set(f"{cfg.soc_curr_1_c:.2f}")
        self.cfg_vars["soc_curr_2_c"].set(f"{cfg.soc_curr_2_c:.2f}")
        self.cfg_vars["soc_curr_3_c"].set(f"{cfg.soc_curr_3_c:.2f}")
        self.cfg_vars["soc_curr_4_c"].set(f"{cfg.soc_curr_4_c:.2f}")
        self.cfg_checks["protect_jack_charge_enabled"].set(cfg.protect_jack_charge_enabled)
        self.cfg_vars["protect_jack_charge_delta_v"].set(f"{cfg.protect_jack_charge_delta_v:.2f}")
        self.cfg_vars["protect_jack_charge_delay_s"].set(str(cfg.protect_jack_charge_delay_s))
        self.cfg_checks["protect_jack_temp_enabled"].set(cfg.protect_jack_temp_enabled)
        self.cfg_vars["protect_jack_temp_delay_s"].set(str(cfg.protect_jack_temp_delay_s))
        self.cfg_vars["protect_jack_temp_threshold_c"].set(f"{cfg.protect_jack_temp_threshold_c:.2f}")
        self.cfg_vars["protect_jack_temp_delta_c"].set(f"{cfg.protect_jack_temp_delta_c:.2f}")
        self.cfg_vars["protect_jack_temp_power_limit_pct"].set(f"{cfg.protect_jack_temp_power_limit_pct:.2f}")
        self.cfg_vars["protect_jack_temp_trip_c"].set(f"{cfg.protect_jack_temp_trip_c:.2f}")
        source_mode_index = sorted(CHARGE_SOURCE_MODE_NAMES).index(cfg.charge_source_mode) if cfg.charge_source_mode in CHARGE_SOURCE_MODE_NAMES else 0
        self.cfg_charge_source_mode.current(source_mode_index)
        self.cfg_vars["can_battery_id"].set(str(cfg.can_battery_id))
        self.cfg_vars["source_module_count"].set(str(cfg.source_module_count))
        self.cfg_vars["module_u_min_v"].set(f"{cfg.module_u_min_v:.2f}")
        self.cfg_vars["module_u_max_v"].set(f"{cfg.module_u_max_v:.2f}")
        self.cfg_vars["module_i_min_a"].set(f"{cfg.module_i_min_a:.2f}")
        self.cfg_vars["module_i_max_a"].set(f"{cfg.module_i_max_a:.2f}")
        module_index = sorted(MODULE_TYPE_NAMES).index(cfg.module_type) if cfg.module_type in MODULE_TYPE_NAMES else 0
        self.cfg_module_type.current(module_index)

    def _read_cfg_float(self, key: str) -> float:
        try:
            return float(self.cfg_vars[key].get().strip())
        except:
            return 0.0

    def _read_cfg_int(self, key: str) -> int:
        try:
            return int(self.cfg_vars[key].get().strip())
        except:
            return 0

    def _collect_charge_config_from_ui(self) -> ChargeCycleConfig:
        module_name = self.cfg_module_type.get()
        module_type = next((idx for idx, name in MODULE_TYPE_NAMES.items() if name == module_name), 0)
        charge_source_name = self.cfg_charge_source_mode.get()
        charge_source_mode = next((idx for idx, name in CHARGE_SOURCE_MODE_NAMES.items() if name == charge_source_name), 0)
        return ChargeCycleConfig(
            version=4,
            battery_capacity_ah=self._read_cfg_float("battery_capacity_ah"),
            imin_c=self._read_cfg_float("imin_c"),
            imax_c=self._read_cfg_float("imax_c"),
            ipre_c=self._read_cfg_float("ipre_c"),
            ilow_c=self._read_cfg_float("ilow_c"),
            vmin_v=self._read_cfg_float("vmin_v"),
            vmax_v=self._read_cfg_float("vmax_v"),
            vpre_v=self._read_cfg_float("vpre_v"),
            vlow_v=self._read_cfg_float("vlow_v"),
            temp_limit_c=self._read_cfg_float("temp_limit_c"),
            cell_volt_enabled=self.cfg_checks["cell_volt_enabled"].get(),
            cell_volt_delta_v=self._read_cfg_float("cell_volt_delta_v"),
            cell_volt_1_v=self._read_cfg_float("cell_volt_1_v"),
            cell_volt_2_v=self._read_cfg_float("cell_volt_2_v"),
            cell_volt_3_v=self._read_cfg_float("cell_volt_3_v"),
            cell_volt_4_v=self._read_cfg_float("cell_volt_4_v"),
            cell_volt_5_v=self._read_cfg_float("cell_volt_5_v"),
            cell_curr_1_c=self._read_cfg_float("cell_curr_1_c"),
            cell_curr_2_c=self._read_cfg_float("cell_curr_2_c"),
            cell_curr_3_c=self._read_cfg_float("cell_curr_3_c"),
            cell_curr_4_c=self._read_cfg_float("cell_curr_4_c"),
            temp_enabled=self.cfg_checks["temp_enabled"].get(),
            temp_delta_c=self._read_cfg_float("temp_delta_c"),
            temp_1_c=self._read_cfg_float("temp_1_c"),
            temp_2_c=self._read_cfg_float("temp_2_c"),
            temp_3_c=self._read_cfg_float("temp_3_c"),
            temp_4_c=self._read_cfg_float("temp_4_c"),
            temp_5_c=self._read_cfg_float("temp_5_c"),
            temp_curr_1_c=self._read_cfg_float("temp_curr_1_c"),
            temp_curr_2_c=self._read_cfg_float("temp_curr_2_c"),
            temp_curr_3_c=self._read_cfg_float("temp_curr_3_c"),
            temp_curr_4_c=self._read_cfg_float("temp_curr_4_c"),
            soc_enabled=self.cfg_checks["soc_enabled"].get(),
            soc_delta_pct=self._read_cfg_float("soc_delta_pct"),
            soc_1_pct=self._read_cfg_float("soc_1_pct"),
            soc_2_pct=self._read_cfg_float("soc_2_pct"),
            soc_3_pct=self._read_cfg_float("soc_3_pct"),
            soc_4_pct=self._read_cfg_float("soc_4_pct"),
            soc_5_pct=self._read_cfg_float("soc_5_pct"),
            soc_curr_1_c=self._read_cfg_float("soc_curr_1_c"),
            soc_curr_2_c=self._read_cfg_float("soc_curr_2_c"),
            soc_curr_3_c=self._read_cfg_float("soc_curr_3_c"),
            soc_curr_4_c=self._read_cfg_float("soc_curr_4_c"),
            protect_jack_charge_enabled=self.cfg_checks["protect_jack_charge_enabled"].get(),
            protect_jack_charge_delta_v=self._read_cfg_float("protect_jack_charge_delta_v"),
            protect_jack_charge_delay_s=self._read_cfg_int("protect_jack_charge_delay_s"),
            protect_jack_temp_enabled=self.cfg_checks["protect_jack_temp_enabled"].get(),
            protect_jack_temp_delay_s=self._read_cfg_int("protect_jack_temp_delay_s"),
            protect_jack_temp_threshold_c=self._read_cfg_float("protect_jack_temp_threshold_c"),
            protect_jack_temp_delta_c=self._read_cfg_float("protect_jack_temp_delta_c"),
            protect_jack_temp_power_limit_pct=self._read_cfg_float("protect_jack_temp_power_limit_pct"),
            protect_jack_temp_trip_c=self._read_cfg_float("protect_jack_temp_trip_c"),
            charge_source_mode=charge_source_mode,
            can_battery_id=self._read_cfg_int("can_battery_id"),
            source_module_count=self._read_cfg_int("source_module_count"),
            module_type=module_type,
            module_u_min_v=self._read_cfg_float("module_u_min_v"),
            module_u_max_v=self._read_cfg_float("module_u_max_v"),
            module_i_min_a=self._read_cfg_float("module_i_min_a"),
            module_i_max_a=self._read_cfg_float("module_i_max_a")
        )

    def _request_charge_config(self):
        if not self.serial.is_connected():
            messagebox.showwarning("Warning", "Connect to port first.")
            return
        payload = struct.pack("<BB", StdCmd.REQ, DebugCmd.CFG_READ)
        self.serial.write_frame(DebugCmd.CFG_READ, payload)

    def _send_charge_config(self):
        if not self.serial.is_connected():
            messagebox.showwarning("Warning", "Connect to port first.")
            return
        try:
            cfg = self._collect_charge_config_from_ui()
            payload = struct.pack("<BB", StdCmd.REQ, DebugCmd.CFG_WRITE) + self.parser.serialize_charge_cfg(cfg)
            self.serial.write_frame(DebugCmd.CFG_WRITE, payload)
            messagebox.showinfo("Success", "Sent configuration to device.")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to send configuration: {e}")

    def _start(self):
        if not self.serial.is_connected():
            messagebox.showwarning("Warning", "Connect to port first.")
            return
        payload = struct.pack("<BB", StdCmd.REQ, DebugCmd.SET_RUN_STATE) + struct.pack("<B", 1)
        self.serial.write_frame(DebugCmd.SET_RUN_STATE, payload)
        messagebox.showinfo("Start", "Start command sent.")

    def _stop(self):
        if not self.serial.is_connected():
            messagebox.showwarning("Warning", "Connect to port first.")
            return
        payload = struct.pack("<BB", StdCmd.REQ, DebugCmd.SET_RUN_STATE) + struct.pack("<B", 0)
        self.serial.write_frame(DebugCmd.SET_RUN_STATE, payload)
        messagebox.showinfo("Stop", "Stop command sent.")

if __name__ == "__main__":
    root = tk.Tk()
    app = ChargerUserApp(root)
    root.mainloop()
