"""
Charger Debug Application - Pixel-Perfect WinForms Layout
1366x768 default, 1280x720 minimum
"""

import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import struct
import time
import csv
import queue
from collections import deque
from typing import List, Dict, Optional
from dataclasses import dataclass

from protocol.debug_protocol import (
    DebugProtocolParser, DebugCmd, DebugRsp,
    StdCmd, DRIVER_NAMES, STATE_NAMES, MODULE_TYPE_NAMES, CHARGE_SOURCE_MODE_NAMES,
    ModuleData, BMSData, ChargeCycleConfig, SystemInfo,
    ALARM_FLAG_NAMES, MAXWELL_ALARM_NAMES, LIANMING_ALARM_NAMES,
    TONHE_STATUS_FAULT_NAMES, TONHE_PFC_FAULT_NAMES
)
from services.serial_service import SerialService


# =============================================================================
# DATA MODELS
# =============================================================================

@dataclass
class AlarmInfo:
    num: int
    timestamp: str
    module: str
    level: str
    message: str


@dataclass
class TrafficEntry:
    time: str
    type: str
    id_hex: str
    dlc: int
    data_hex: str
    module: str
    info: str


@dataclass
class ChargerModule:
    """Internal module representation"""
    addr: int
    driver: int
    group: int = 0
    online: bool = False
    running: bool = False
    state: int = 0
    user_added: bool = False  # True if user clicked Add; False if auto-discovered from MCU
    voltage: Optional[float] = None
    current: Optional[float] = None
    current_limit: Optional[float] = None
    input_power: Optional[float] = None
    rated_power: Optional[float] = None
    rated_current: Optional[float] = None
    temp_dcdc: Optional[float] = None
    temp_ambient: Optional[float] = None
    temp_pfc: Optional[float] = None
    ac_phase_a: Optional[float] = None
    ac_phase_b: Optional[float] = None
    ac_phase_c: Optional[float] = None
    pfc_bus_plus: Optional[float] = None
    pfc_bus_minus: Optional[float] = None
    alarm_flags: int = 0
    status_flags: int = 0
    pfc_fault: int = 0
    tx_count: int = 0
    rx_count: int = 0
    error_count: int = 0
    timeout_count: int = 0
    recovery_count: int = 0
    last_update: Optional[float] = None
    module_idx: int = 0

    def get_driver_name(self) -> str:
        return DRIVER_NAMES.get(self.driver, "Unknown")

    def get_state_name(self) -> str:
        return STATE_NAMES.get(self.state, f"Unknown({self.state})")

    def get_alarm_names(self) -> List[str]:
        alarms: List[str] = []

        def append_unique(name: str):
            if name and name not in alarms:
                alarms.append(name)

        if self.driver == 1 and self.status_flags:
            for bit, name in MAXWELL_ALARM_NAMES.items():
                if self.status_flags & (1 << bit):
                    append_unique(name)
        elif self.driver == 2 and (self.status_flags & 0xFFFF):
            for bit, name in LIANMING_ALARM_NAMES.items():
                if self.status_flags & (1 << bit):
                    append_unique(name)
        elif self.driver == 3 and (self.status_flags & 0xFFFF):
            for bit, name in TONHE_STATUS_FAULT_NAMES.items():
                if self.status_flags & (1 << bit):
                    append_unique(name)
        if self.driver == 3 and self.pfc_fault:
            for bit, name in TONHE_PFC_FAULT_NAMES.items():
                if self.pfc_fault & (1 << bit):
                    append_unique(f"PFC: {name}")
        if self.alarm_flags:
            for bit, name in ALARM_FLAG_NAMES.items():
                if self.alarm_flags & (1 << bit):
                    append_unique(name)
        if self.state == 4:
            append_unique("State offline")
        elif self.state == 5:
            append_unique("State fault")
        return alarms

    def get_alarm_summary(self) -> str:
        alarms = self.get_alarm_names()
        return "; ".join(alarms) if alarms else "None"


# =============================================================================
# MAIN APPLICATION
# =============================================================================

class ChargerDebugApp:
    def __init__(self, root):
        self.root = root
        self.root.title("Charger Debug App - UI v3 BMS")
        self.root.update_idletasks()
        self.screen_width = self.root.winfo_screenwidth()
        self.screen_height = self.root.winfo_screenheight()
        # Keep Tk's native DPI awareness, then apply a bounded screen-size
        # adjustment so the same layout remains usable on laptops, desktop
        # monitors and high-DPI displays.
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
        available_width = max(760, self.screen_width - 40)
        available_height = max(560, self.screen_height - 100)
        default_width = min(1366, available_width, max(860, int(self.screen_width * 0.90)))
        default_height = min(768, available_height, max(560, int(self.screen_height * 0.86)))
        min_width = min(980, available_width)
        min_height = min(620, available_height)
        self.root.geometry(f"{default_width}x{default_height}")
        self.root.minsize(min_width, min_height)
        self.root.state("zoomed")
        self._configure_styles()

        # Services
        self.serial = SerialService()
        self.parser = DebugProtocolParser()

        # State
        self.modules: Dict[int, ChargerModule] = {}
        self.selected_module_idx: Optional[int] = None
        self.driver_id = 1
        self.alarms: List[AlarmInfo] = []
        self.traffic: deque = deque(maxlen=500)
        self.traffic_filter = {"TX": True, "RX": True, "SYS": True, "ERROR": True, "WARN": True}
        self.next_alarm_num = 1
        self.log_entries: List[str] = []
        self.bms_data: Optional[BMSData] = None
        self.system_info: Optional[SystemInfo] = None
        self.charge_config: Optional[ChargeCycleConfig] = None
        self.active_alarm_keys = set()
        self._syncing_module_selection = False
        # (addr, driver_id) keys the user explicitly clicked Remove/Clear
        # for -- the wire protocol has no "un-register module" command, so
        # the MCU keeps reporting these via ALL_MODULES; this set tells
        # _update_module_from_data()'s auto-discovery path to leave them
        # alone instead of silently recreating the row. Cleared for a given
        # key the moment the user re-Adds it (see _add_module()).
        self._user_removed_keys: set = set()

        # Charge Graph tab state — bounded ring buffer of (timestamp, voltage,
        # current) samples for the currently-selected module, ~10 minutes at
        # the MCU's ~1s telemetry cadence. Populated from
        # _update_module_from_data() (real telemetry arrival), never from the
        # GUI tick, and only redrawn while the tab is actually visible — see
        # _append_charge_graph_sample()/_redraw_charge_graph().
        self.charge_graph_history: deque = deque(maxlen=600)
        self._charge_graph_module_idx: Optional[int] = None
        self._charge_graph_built = False
        self._charge_graph_active = False
        # Wall-clock time of the first sample since the last clear/selection
        # change -- the X axis shows elapsed time since THIS, not since
        # whatever happens to be the oldest point still in the (bounded)
        # deque, so a long session's axis keeps meaning "time since this
        # charge started" even after old points have rolled out of view.
        self._charge_graph_t0: Optional[float] = None

        # Throttling state
        self._ui_dirty = False
        self._traffic_dirty = False
        # Timestamp of the last window resize or mousewheel-scroll event
        # (see _on_root_configure()/_bind_mousewheel_to_canvas()).
        # _gui_update_loop() checks this and skips its widget-content-update
        # pass while an interaction is still in progress -- Tk's own
        # geometry manager is already repainting the window during a resize
        # or scroll drag, and layering the app's independent ~60fps
        # .configure()/treeview-insert churn on top of that at the same time
        # is what read as screen tearing across every tab, not just charts
        # (confirmed with the user 2026-08-29: happens on any page, not
        # just the one canvas-based tab). The queued frames are still
        # drained/coalesced every tick either way, so no telemetry is lost
        # -- only applying it to widgets is deferred until the interaction
        # settles.
        self._last_interaction_time = 0.0
        self._pending_traffic_inserts = []
        self._user_scrolling_traffic = False
        self._error_count = 0  # Cache error count to avoid O(n) sum every cycle
        self._last_timestamp = ""
        self._last_timestamp_update = 0  # For timestamp caching
        self._module_lookup = {}  # {(addr, driver): idx} for O(1) lookup
        self._frame_queue = queue.Queue()  # Thread-safe queue for serial frames
        # MCU auto-pushes ALL_MODULES/SYSTEM_INFO/BMS_DATA every 1s once ENTER
        # is sent (DebugProtocol_SendStream) — these timestamps just track
        # freshness for the UI, they don't drive any request scheduler.
        self._last_module_rx = 0.0
        self._last_system_rx = 0.0
        self._last_bms_rx = 0.0
        self._monitor_stale_ms = 3000
        self._last_bms_alarm_flags = None  # Cache for BMS alarm dirty check

        # Frame coalescing for GUI loop (Fix 5)
        self._latest_frames: Dict[int, bytes] = {}

        # UI state
        self.detail_vars: Dict[str, tk.StringVar] = {}
        self.stats_vars: Dict[str, tk.StringVar] = {}
        self.bms_vars: Dict[str, tk.StringVar] = {}
        self.process_vars: Dict[str, tk.StringVar] = {}
        self.cfg_vars: Dict[str, tk.StringVar] = {}
        self.cfg_checks: Dict[str, tk.BooleanVar] = {}
        self._pending_charge_config = None  # Config received before tab is built

        # Build UI
        self._build_ui()
        self._update_bms_detail()

        # Register callbacks
        self.serial.on_frame(self._on_frame)
        self.serial.on_log(self._on_log)

        # Refresh ports
        self._refresh_ports()

        # Keyboard shortcuts
        self.root.bind("<F5>", lambda e: self._start())
        self.root.bind("<F6>", lambda e: self._stop())
        self.root.bind("<F9>", lambda e: self._estop())
        self.root.bind("<Control-r>", lambda e: self._refresh_ports())
        # See _last_interaction_time's docstring in __init__ -- this is the
        # resize half of the anti-tearing throttle; the scroll half is in
        # _bind_mousewheel_to_canvas().
        self.root.bind("<Configure>", self._on_root_configure)

        # Start UI loop
        self.root.after(100, self._gui_update_loop)

    def _responsive_min_width(self, preferred: int) -> int:
        """Keep wide desktop layouts usable without forcing a huge viewport."""
        return max(820, min(preferred, self.screen_width - 40))

    def _on_root_configure(self, _event=None):
        """Root window resize -- see _last_interaction_time in __init__."""
        self._last_interaction_time = time.perf_counter()

    # How long after the last resize/scroll event _gui_update_loop() keeps
    # deferring its widget-content-update pass. Long enough to cover the
    # gap between individual events in a continuous drag/scroll (they fire
    # much faster than this), short enough that releasing the mouse still
    # feels instant.
    INTERACTION_SETTLE_S = 0.15

    def _gui_update_loop(self):
        """GUI update loop with coalescing + 8ms time budget to keep UI responsive."""
        try:
            loop_start = time.perf_counter()
            deadline = loop_start + 0.008  # 8ms budget per cycle

            # Drain queue with coalescing — keep only latest frame per response type
            while not self._frame_queue.empty():
                if time.perf_counter() >= deadline:
                    break
                try:
                    cmd, payload = self._frame_queue.get_nowait()
                    self._latest_frames[cmd] = payload  # coalesce: overwrite older frames
                except queue.Empty:
                    break

            # Process coalesced frames — always clear dict even on exception
            frames_to_process = list(self._latest_frames.items())
            self._latest_frames.clear()
            for cmd, payload in frames_to_process:
                try:
                    self._process_frame(cmd, payload)
                except Exception as exc:
                    print(f"[GUI] _process_frame error cmd=0x{cmd:02X}: {exc}")

            # Defer all widget-content updates while a resize or scroll is
            # still settling (see _last_interaction_time in __init__) --
            # Tk's own geometry manager / the OS compositor is already
            # repainting the window for that resize/scroll right now, and
            # piling this loop's independent ~60fps .configure()/treeview
            # churn on top of it at the same moment is what read as
            # tearing across every tab, not just one. Frames stay queued
            # (_ui_dirty/_traffic_dirty stay True) so nothing is lost --
            # this only pushes back *applying* it to widgets a few ticks.
            interacting = (time.perf_counter() - self._last_interaction_time) < self.INTERACTION_SETTLE_S

            if self._ui_dirty and not interacting:
                self._update_module_grid()
                self._update_detail()
                self._update_charge_process()
                self._update_bms_detail()
                self._check_alarms()
                self._ui_dirty = False

            if self._traffic_dirty and not interacting:
                # Skip traffic insert/delete if user is scrolling to prevent tearing
                # But still update count labels
                if not self._user_scrolling_traffic:
                    last_item = None
                    for entry in self._pending_traffic_inserts:
                        if entry.type not in self.traffic_filter or self.traffic_filter[entry.type]:
                            last_item = self.tree_traffic.insert("", "end", values=(
                                entry.time, entry.type, entry.id_hex, entry.dlc, entry.data_hex, entry.info
                            ), tags=(entry.type,))

                    self._pending_traffic_inserts.clear()

                    # Keep treeview size in check - reduce to 100 to prevent UI tearing
                    children = self.tree_traffic.get_children()
                    if len(children) > 100:
                        self.tree_traffic.delete(*children[:-100])

                    # Only auto-scroll if user enabled it and not currently scrolling
                    if last_item and hasattr(self, 'chk_auto_scroll') and self.chk_auto_scroll.get():
                        self.tree_traffic.see(last_item)

                self.lbl_traffic_count.configure(text=f"Log Lines: {len(self.traffic)}")
                self.lbl_error_count.configure(text=f"Errors: {self._error_count}")
                
                self._traffic_dirty = False
                
        finally:
            self.root.after(16, self._gui_update_loop)  # ~60fps max — coalescing prevents overload

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

    # =========================================================================
    # UI BUILD - ROOT STRUCTURE
    # =========================================================================

    def _build_ui(self):
        """Build complete UI - matches WinForms layout exactly"""
        # Root frame with 8px padding
        root = ttk.Frame(self.root)
        root.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)
        root.grid_columnconfigure(0, weight=1)
        root.grid_rowconfigure(1, weight=1)

        # Row 0: Connection bar (fixed height ~50px)
        self._build_connection_bar(root)

        # Row 1: Explicit page switcher so tabs are always visible
        self._build_page_switcher(root)

        # Row 2: Main content area (expandable)
        self.page_host = ttk.Frame(root)
        self.page_host.pack(fill=tk.BOTH, expand=True, pady=(6, 0))
        self.page_host.grid_columnconfigure(0, weight=1)
        self.page_host.grid_rowconfigure(0, weight=1)

        control_tab = ttk.Frame(self.page_host)
        control_tab.grid_columnconfigure(0, weight=1)
        control_tab.grid_rowconfigure(0, weight=1)
        self.page_control = control_tab

        monitor_tab = ttk.Frame(self.page_host)
        monitor_tab.grid_columnconfigure(0, weight=1)
        monitor_tab.grid_rowconfigure(0, weight=1)
        self.page_monitor = monitor_tab

        config_tab = ttk.Frame(self.page_host)
        config_tab.grid_columnconfigure(0, weight=1)
        config_tab.grid_rowconfigure(0, weight=1)
        self.page_charge_config = config_tab

        graph_tab = ttk.Frame(self.page_host)
        graph_tab.grid_columnconfigure(0, weight=1)
        # Unlike the other 3 tabs (single content row), this tab has a
        # fixed-height header row (0) plus an expanding canvas row (1) --
        # row-weight config for both lives in _build_charge_graph_tab()
        # itself, not here, so row 0 doesn't also stretch.
        self.page_charge_graph = graph_tab

        self.page_control.grid(row=0, column=0, sticky="nsew")
        self.page_monitor.grid(row=0, column=0, sticky="nsew")
        self.page_charge_config.grid(row=0, column=0, sticky="nsew")
        self.page_charge_graph.grid(row=0, column=0, sticky="nsew")

        self._build_monitor_tab(monitor_tab)
        self._build_control_tab(control_tab)
        # Charge Config and Charge Graph tabs are lazy-loaded on first switch
        self._charge_config_built = False
        self._charge_graph_built = False
        self._set_active_tab(0)

        # Row 3: Status bar (28px fixed)
        self._build_status_bar(root)

    def _build_page_switcher(self, parent):
        """Visible page switcher above notebook content."""
        bar = tk.Frame(parent, bg="#F0F0F0")
        bar.pack(fill=tk.X, pady=(8, 0))

        tabs = tk.Frame(bar, bg="#F0F0F0")
        tabs.pack(side=tk.LEFT, anchor="w")

        self.tab_baseline = tk.Frame(bar, height=1, bg="#B8B8B8")
        self.tab_baseline.pack(side=tk.BOTTOM, fill=tk.X)

        self.btn_tab_control = tk.Button(
            tabs,
            text="Control",
            command=lambda: self._set_active_tab(0),
            font=("Segoe UI", 9),
            relief="solid",
            bd=1,
            padx=18,
            pady=5,
            cursor="hand2",
            highlightthickness=0,
        )
        self.btn_tab_control.pack(side=tk.LEFT, padx=(0, 2))

        self.btn_tab_monitor = tk.Button(
            tabs,
            text="Monitor",
            command=lambda: self._set_active_tab(1),
            font=("Segoe UI", 9),
            relief="solid",
            bd=1,
            padx=18,
            pady=5,
            cursor="hand2",
            highlightthickness=0,
        )
        self.btn_tab_monitor.pack(side=tk.LEFT)

        self.btn_tab_charge_config = tk.Button(
            tabs,
            text="Charge Config",
            command=lambda: self._set_active_tab(2),
            font=("Segoe UI", 9),
            relief="solid",
            bd=1,
            padx=18,
            pady=5,
            cursor="hand2",
            highlightthickness=0,
        )
        self.btn_tab_charge_config.pack(side=tk.LEFT, padx=(2, 0))

        self.btn_tab_charge_graph = tk.Button(
            tabs,
            text="Charge Graph",
            command=lambda: self._set_active_tab(3),
            font=("Segoe UI", 9),
            relief="solid",
            bd=1,
            padx=18,
            pady=5,
            cursor="hand2",
            highlightthickness=0,
        )
        self.btn_tab_charge_graph.pack(side=tk.LEFT, padx=(2, 0))

    def _set_active_tab(self, index: int):
        """Switch visible page via tkraise().

        Reverted 2026-08-29: briefly tried grid_remove()/grid() here
        instead (unmapping inactive pages entirely, on the theory that
        having all 4 pages still gridded in the same cell was forcing Tk
        to re-lay-out all of them on every resize). Confirmed with the
        user that this made things WORSE, not better -- black patches
        appearing specifically on shrink-then-grow, i.e. regions Tk didn't
        repaint promptly after a page got unmapped/remapped mid-resize.
        Back to tkraise()-only, matching the original author's own
        "no grid_remove/grid to avoid flicker" comment -- they'd already
        been here. The remaining rendering-during-resize/scroll issue is
        addressed instead by throttling this app's OWN widget-content
        updates during an active interaction (see _last_interaction_time /
        INTERACTION_SETTLE_S in _gui_update_loop()), which doesn't touch
        widget mapping/geometry at all and is lower-risk.
        """
        # Lazy-load Charge Config tab on first access (Fix 2)
        if index == 2 and not self._charge_config_built:
            self._build_charge_config_tab(self.page_charge_config)
            self._charge_config_built = True
            # Scrollregion is now handled by the event binding on the inner frame
            # Apply any config received from MCU before the tab was built
            if self._pending_charge_config is not None:
                self._load_charge_config_to_ui(self._pending_charge_config)
                self._pending_charge_config = None

        # Lazy-load Charge Graph tab on first access — same reasoning as
        # Charge Config: don't pay for the canvas/history buffers until the
        # operator actually opens this tab.
        if index == 3 and not self._charge_graph_built:
            self._build_charge_graph_tab(self.page_charge_graph)
            self._charge_graph_built = True

        # The graph only needs to redraw while it's the visible page — track
        # that here so _append_charge_graph_sample() can skip render work
        # entirely for every sample that arrives while another tab is shown.
        self._charge_graph_active = (index == 3)
        if self._charge_graph_active and self._charge_graph_built:
            self._redraw_charge_graph()

        pages = (self.page_control, self.page_monitor, self.page_charge_config, self.page_charge_graph)
        if 0 <= index < len(pages):
            pages[index].tkraise()
        titles = (
            "Charger Debug App - UI v3 BMS - Control Page",
            "Charger Debug App - UI v3 BMS - Monitor Page",
            "Charger Debug App - UI v3 BMS - Charge Config Page",
            "Charger Debug App - UI v3 BMS - Charge Graph Page",
        )
        self.root.title(titles[index])
        self._update_tab_buttons(index)

    def _update_tab_buttons(self, active_index: int):
        """Highlight current page button."""
        active_bg = "#FFFFFF"
        active_fg = "#000000"
        inactive_bg = "#ECECEC"
        inactive_fg = "#444444"
        border = "#B8B8B8"
        self.btn_tab_control.configure(
            bg=active_bg if active_index == 0 else inactive_bg,
            fg=active_fg if active_index == 0 else inactive_fg,
            relief="solid",
            borderwidth=1,
            activebackground=active_bg if active_index == 0 else "#F4F4F4",
            activeforeground=active_fg if active_index == 0 else "#222222",
            highlightbackground=border,
        )
        self.btn_tab_monitor.configure(
            bg=active_bg if active_index == 1 else inactive_bg,
            fg=active_fg if active_index == 1 else inactive_fg,
            relief="solid",
            borderwidth=1,
            activebackground=active_bg if active_index == 1 else "#F4F4F4",
            activeforeground=active_fg if active_index == 1 else "#222222",
            highlightbackground=border,
        )
        self.btn_tab_charge_config.configure(
            bg=active_bg if active_index == 2 else inactive_bg,
            fg=active_fg if active_index == 2 else inactive_fg,
            relief="solid",
            borderwidth=1,
            activebackground=active_bg if active_index == 2 else "#F4F4F4",
            activeforeground=active_fg if active_index == 2 else "#222222",
            highlightbackground=border,
        )
        self.btn_tab_charge_graph.configure(
            bg=active_bg if active_index == 3 else inactive_bg,
            fg=active_fg if active_index == 3 else inactive_fg,
            relief="solid",
            borderwidth=1,
            activebackground=active_bg if active_index == 3 else "#F4F4F4",
            activeforeground=active_fg if active_index == 3 else "#222222",
            highlightbackground=border,
        )

    # =========================================================================
    # CONNECTION BAR
    # =========================================================================

    def _build_connection_bar(self, parent):
        """Connection bar - matches spec: Port | Refresh | Connect | Sep | Baudrate | Sep | Status"""
        frm = ttk.Frame(parent, height=50)
        frm.pack(fill=tk.X)
        frm.pack_propagate(False)

        # Port selection
        ttk.Label(frm, text="Port:").pack(side=tk.LEFT, anchor="center", padx=(10, 0))
        self.cmb_port = ttk.Combobox(frm, width=12, state="readonly")
        self.cmb_port.pack(side=tk.LEFT, padx=(8, 4))

        ttk.Button(frm, text="Refresh", command=self._refresh_ports, width=8).pack(side=tk.LEFT, padx=2)
        self.btn_connect = ttk.Button(frm, text="Connect", command=self._toggle_connect, width=10)
        self.btn_connect.pack(side=tk.LEFT, padx=2)

        # Separator
        sep1 = ttk.Separator(frm, orient="vertical")
        sep1.pack(side=tk.LEFT, fill=tk.Y, padx=8, pady=5)

        # Baudrate
        ttk.Label(frm, text="Baudrate:", foreground="gray").pack(side=tk.LEFT)
        ttk.Label(frm, text="115200").pack(side=tk.LEFT, padx=(4, 0))

        # Separator
        sep2 = ttk.Separator(frm, orient="vertical")
        sep2.pack(side=tk.LEFT, fill=tk.Y, padx=8, pady=5)

        # Status
        self.lbl_status = tk.Label(frm, text="Disconnected", fg="red", font=("Segoe UI", 9, "bold"), bg="#F0F0F0")
        self.lbl_status.pack(side=tk.LEFT, padx=(4, 0))

        # Spacer
        tk.Frame(frm).pack(side=tk.LEFT, fill=tk.X, expand=True)

        # Raw serial debug log toggle -- off by default (see
        # services/serial_service.py's SerialService.set_debug_enabled()
        # docstring: this used to be a hardcoded-True constant that opened,
        # wrote, and closed serial_debug.log on every single TX/RX frame on
        # the serial RX thread, which was the actual root cause behind
        # reports of the app feeling laggy). Only turn it on when actually
        # diagnosing a comms issue.
        self.var_debug_log = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            frm, text="Serial Debug Log", variable=self.var_debug_log,
            command=lambda: self.serial.set_debug_enabled(self.var_debug_log.get()),
        ).pack(side=tk.RIGHT, padx=(0, 10))

    # =========================================================================
    # LEFT PANEL
    # =========================================================================

    def _build_left_panel(self, parent):
        """Left panel: Module Management + Commands"""
        parent.grid_columnconfigure(0, weight=1)
        parent.grid_rowconfigure(0, weight=1)

        group = tk.LabelFrame(parent, text="Module Controls", font=("Segoe UI", 9),
                              padx=8, pady=6, bg="#F0F0F0")
        group.grid(row=0, column=0, sticky="nsew")
        group.grid_columnconfigure(0, weight=1)
        group.grid_rowconfigure(0, weight=0)
        group.grid_rowconfigure(1, weight=1)

        # Module Management (fixed ~280px height)
        self._build_module_management(group)

        # Commands (fills remaining space)
        self._build_commands(group)

    def _build_module_management(self, parent):
        """Module Management panel"""
        frm = tk.LabelFrame(parent, text="Module Management", font=("Segoe UI", 9),
                            padx=8, pady=4, bg="#F0F0F0")
        frm.grid(row=0, column=0, sticky="new", pady=(0, 6))

        inner = ttk.Frame(frm)
        inner.pack(fill=tk.X, pady=(4, 0))

        # Address input row
        ttk.Label(inner, text="Address:").grid(row=0, column=0, sticky="w")
        self.ent_addr = ttk.Entry(inner, width=10)
        self.ent_addr.grid(row=0, column=1, padx=(4, 0))
        self.ent_addr.insert(0, "0x01")
        ttk.Button(inner, text="Add", command=self._add_module, width=5).grid(row=0, column=2, padx=(4, 0))

        ttk.Label(inner, text="Driver:").grid(row=1, column=0, sticky="w", pady=(6, 0))
        self.cmb_driver = ttk.Combobox(
            inner,
            width=14,
            state="readonly",
            values=["Maxwell", "Lianming", "TonHe"],
        )
        self.cmb_driver.grid(row=1, column=1, columnspan=2, sticky="ew", padx=(4, 0), pady=(6, 0))
        self.cmb_driver.current(0)
        self.cmb_driver.bind("<<ComboboxSelected>>", self._on_driver_change)

        # Modules list (Treeview)
        columns = ("addr", "driver", "online")
        self.tree_modules = ttk.Treeview(frm, columns=columns, show="headings", height=3)
        self.tree_modules.heading("addr", text="Addr")
        self.tree_modules.heading("driver", text="Driver")
        self.tree_modules.heading("online", text="Online")
        self.tree_modules.column("addr", width=50, anchor="center")
        self.tree_modules.column("driver", width=70, anchor="center")
        self.tree_modules.column("online", width=50, anchor="center")
        self.tree_modules.pack(fill=tk.X, pady=(8, 0))
        self.tree_modules.bind("<<TreeviewSelect>>", self._on_module_select)
        # Tag colors for module state
        self.tree_modules.tag_configure("online", foreground="#10B981")     # Green
        self.tree_modules.tag_configure("offline", foreground="#9CA3AF")    # Gray
        self.tree_modules.tag_configure("fault", foreground="#EF4444")       # Red
        self.tree_modules.tag_configure("running", foreground="#10B981")    # Green

        # Buttons row
        btn_row = ttk.Frame(frm)
        btn_row.pack(fill=tk.X, pady=(4, 0))
        ttk.Button(btn_row, text="Remove", command=self._remove_module).pack(side=tk.LEFT, expand=True, fill=tk.X, padx=(0, 2))
        ttk.Button(btn_row, text="Clear", command=self._clear_modules).pack(side=tk.LEFT, expand=True, fill=tk.X, padx=(2, 0))

    def _build_commands(self, parent):
        """Commands panel"""
        frm = tk.LabelFrame(parent, text="Commands", font=("Segoe UI", 9),
                            padx=8, pady=4, bg="#F0F0F0")
        frm.grid(row=1, column=0, sticky="nsew", pady=(2, 0))
        frm.grid_columnconfigure(0, weight=1)

        self.var_manual_mode = tk.BooleanVar(value=False)
        self.chk_manual = ttk.Checkbutton(frm, text="Manual Mode", variable=self.var_manual_mode, command=self._toggle_manual_mode)
        self.chk_manual.pack(fill=tk.X, pady=(0, 4))

        inner = ttk.Frame(frm)
        inner.pack(fill=tk.X, pady=(4, 0))

        # Voltage row
        ttk.Label(inner, text="Voltage:").grid(row=0, column=0, sticky="w")
        self.ent_voltage = ttk.Entry(inner, width=10)
        self.ent_voltage.grid(row=0, column=1, padx=(4, 0))
        self.ent_voltage.insert(0, "54.6")
        self.btn_set_v = ttk.Button(inner, text="Set V", command=self._set_voltage, width=6)
        self.btn_set_v.grid(row=0, column=2, padx=(4, 0))

        # Current row
        ttk.Label(inner, text="Current:").grid(row=1, column=0, sticky="w", pady=(4, 0))
        self.ent_current = ttk.Entry(inner, width=10)
        self.ent_current.grid(row=1, column=1, padx=(4, 0), pady=(4, 0))
        self.ent_current.insert(0, "20.0")
        self.btn_set_i = ttk.Button(inner, text="Set I", command=self._set_current, width=6)
        self.btn_set_i.grid(row=1, column=2, padx=(4, 0), pady=(4, 0))

        # START button (F5)
        self.btn_start = tk.Button(frm, text="▶ START (F5)", command=self._start,
                                   bg="#DFF5E3", fg="#1B5E20", relief="solid", bd=1,
                                   highlightthickness=0, activebackground="#D4EED9",
                                   font=("Segoe UI", 10, "bold"), height=2, cursor="hand2")
        self.btn_start.pack(fill=tk.X, pady=(12, 4), padx=0)

        # STOP button (F6)
        self.btn_stop = tk.Button(frm, text="⏹ STOP (F6)", command=self._stop,
                                  bg="#FFF4CC", fg="#F57F17", relief="solid", bd=1,
                                  highlightthickness=0, activebackground="#FCEAB0",
                                  font=("Segoe UI", 10, "bold"), height=2, cursor="hand2")
        self.btn_stop.pack(fill=tk.X, pady=(0, 4), padx=0)

        # EMERGENCY STOP button - prominent for safety
        self.btn_estop = tk.Button(frm, text="⚠️ EMERGENCY STOP (F9)", command=self._estop,
                                   bg="#DC2626", fg="#FFFFFF", relief="raised", bd=2,
                                   highlightthickness=1, activebackground="#B91C1C",
                                   font=("Segoe UI", 11, "bold"), height=2, cursor="hand2")
        self.btn_estop.pack(fill=tk.X, pady=(8, 4), padx=0)

        self._toggle_manual_mode()

    def _toggle_manual_mode(self):
        state = tk.NORMAL if self.var_manual_mode.get() else tk.DISABLED
        self.ent_voltage.config(state=state)
        self.ent_current.config(state=state)
        self.btn_set_v.config(state=state)
        self.btn_set_i.config(state=state)

    # =========================================================================
    # RIGHT PANEL
    # =========================================================================

    def _build_scrollable_page(self, parent, min_width: int = 1100):
        wrapper = tk.Frame(parent, bg="#F0F0F0")
        wrapper.grid(row=0, column=0, sticky="nsew")
        wrapper.grid_columnconfigure(0, weight=1)
        wrapper.grid_rowconfigure(0, weight=1)

        canvas = tk.Canvas(wrapper, bg="#F0F0F0", highlightthickness=0)
        canvas.grid(row=0, column=0, sticky="nsew")
        v_scroll = ttk.Scrollbar(wrapper, orient="vertical", command=canvas.yview)
        v_scroll.grid(row=0, column=1, sticky="ns")
        h_scroll = ttk.Scrollbar(wrapper, orient="horizontal", command=canvas.xview)
        h_scroll.grid(row=1, column=0, sticky="ew")
        canvas.configure(yscrollcommand=v_scroll.set, xscrollcommand=h_scroll.set)

        body = tk.Frame(canvas, bg="#F0F0F0")
        body_window = canvas.create_window((0, 0), window=body, anchor="nw")

        def sync_scrollregion(_event=None):
            def do_sync():
                canvas.configure(scrollregion=canvas.bbox("all"))
                canvas._sync_scheduled = False
            if not getattr(canvas, '_sync_scheduled', False):
                canvas._sync_scheduled = True
                canvas.after_idle(do_sync)

        def sync_width(event):
            target_width = max(event.width, min_width)
            current_w = canvas.itemconfigure(body_window, 'width')
            # itemconfigure returns a dict on some Tk versions, extract actual value
            if isinstance(current_w, dict):
                current_w = current_w.get('value', target_width)
            if current_w != target_width:
                canvas.itemconfigure(body_window, width=target_width)
            sync_scrollregion()

        body.bind("<Configure>", sync_scrollregion)
        canvas.bind("<Configure>", sync_width)

        # --- MouseWheel support for smooth scrolling ---
        self._bind_mousewheel_to_canvas(canvas, body)

        return body

    def _bind_mousewheel_to_canvas(self, canvas, body):
        """Bind mouse wheel scrolling to a canvas and all its children.
        Uses per-widget binding instead of bind_all to avoid conflicts
        between multiple scrollable canvases."""
        def _on_mousewheel(event):
            # See _last_interaction_time in __init__ -- scroll half of the
            # anti-tearing throttle (resize half is _on_root_configure()).
            self._last_interaction_time = time.perf_counter()
            # Windows: event.delta is typically +/-120
            canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
            return "break"  # Prevent event propagation

        def _bind_to_widget_tree(widget):
            """Recursively bind mousewheel to widget and all children."""
            widget.bind("<MouseWheel>", _on_mousewheel)
            for child in widget.winfo_children():
                _bind_to_widget_tree(child)

        # Bind after widgets are created (delay to ensure children exist)
        canvas.bind("<MouseWheel>", _on_mousewheel)
        canvas.after(200, lambda: _bind_to_widget_tree(body))

    def _build_section_shell(self, parent, row: int, column: int, title: str, subtitle: str = "",
                             columnspan: int = 1, padx: tuple[int, int] = (0, 0),
                             pady: tuple[int, int] = (0, 0)):
        shell = tk.Frame(parent, bg="#FFFFFF", bd=1, relief="solid")
        shell.grid(row=row, column=column, columnspan=columnspan, sticky="nsew", padx=padx, pady=pady)
        shell.grid_columnconfigure(0, weight=1)
        shell.grid_rowconfigure(1, weight=1)

        header = tk.Frame(shell, bg="#E9EEF5")
        header.grid(row=0, column=0, sticky="ew")
        tk.Label(header, text=title, font=("Segoe UI", 10, "bold"),
                 fg="#1F3A5F", bg="#E9EEF5", anchor="w").pack(fill=tk.X, padx=12, pady=(8, 0))
        if subtitle:
            tk.Label(header, text=subtitle, font=("Segoe UI", 8),
                     fg="#5B6B7F", bg="#E9EEF5", anchor="w").pack(fill=tk.X, padx=12, pady=(0, 8))
        else:
            tk.Frame(header, bg="#E9EEF5", height=8).pack(fill=tk.X)

        body = tk.Frame(shell, bg="#FFFFFF")
        body.grid(row=1, column=0, sticky="nsew")
        return shell, body

    def _build_control_tab(self, parent):
        """Control tab: target selection and manual commands."""
        page = self._build_scrollable_page(parent, min_width=self._responsive_min_width(1080))
        page.grid_columnconfigure(0, weight=0, minsize=300)
        page.grid_columnconfigure(1, weight=1)
        page.grid_rowconfigure(0, weight=1)

        left = ttk.Frame(page, width=300)
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        self._build_left_panel(left)

        right = tk.Frame(page, bg="#F0F0F0")
        right.grid(row=0, column=1, sticky="nsew")
        right.grid_columnconfigure(0, weight=1)
        right.grid_rowconfigure(0, weight=1)

        body = tk.Frame(right, bg="#F0F0F0")
        body.grid(row=0, column=0, sticky="nsew")
        body.grid_columnconfigure(0, weight=3, uniform="control_cols")
        body.grid_columnconfigure(1, weight=2, uniform="control_cols")
        body.grid_rowconfigure(0, weight=1)

        _, focus = self._build_section_shell(
            body, 0, 0, "Selected Module Focus",
            "Live context for the current command target.",
            padx=(0, 6)
        )
        self._build_control_focus_panel(focus)

        _, guide = self._build_section_shell(
            body, 0, 1, "Operator Guide",
            "Short rules for predictable manual control.",
            padx=(6, 0)
        )
        self._build_control_notes(guide)

    def _build_monitor_tab(self, parent):
        """Monitor tab: charger telemetry, BMS data, alarms, and traffic."""
        page = self._build_scrollable_page(parent, min_width=self._responsive_min_width(1180))
        # Give BMS Monitor more usable width while keeping charger telemetry
        # as the primary pane: Charger 4 / BMS 3.
        page.grid_columnconfigure(0, weight=4, uniform="monitor_cols")
        page.grid_columnconfigure(1, weight=3, uniform="monitor_cols")
        page.grid_rowconfigure(0, weight=1)
        page.grid_rowconfigure(1, weight=1)

        left = tk.Frame(page, bg="#F0F0F0")
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        left.grid_columnconfigure(0, weight=1)
        left.grid_rowconfigure(0, weight=1)
        left.grid_rowconfigure(1, weight=1)

        _, charger_group = self._build_section_shell(
            left, 0, 0, "Charger Monitor",
            "Observed charger modules and detailed charger telemetry.",
            pady=(0, 8)
        )
        charger_group.grid_columnconfigure(0, weight=1)
        charger_group.grid_rowconfigure(2, weight=1)

        self._build_monitor_module_list(charger_group)

        detail = tk.LabelFrame(charger_group, text="Module Detail", font=("Segoe UI", 9),
                               padx=8, pady=4, bg="#FFFFFF")
        detail.grid(row=2, column=0, sticky="nsew", pady=(8, 8), padx=10)
        self._build_module_detail(detail)

        note = tk.Label(left, text="Monitor keeps charger telemetry, BMS feedback, alarms, and serial traffic in one read-only workspace.",
                        font=("Segoe UI", 8), fg="#666666", bg="#F0F0F0", anchor="w", justify=tk.LEFT)
        note.grid(row=1, column=0, sticky="ew")

        self._build_bms_monitor_panel(page, column=1)

        _, alarm_group = self._build_section_shell(
            page, 1, 0, "Active Alarms",
            "Current active faults only. Use Communication Log for alarm history and protocol events.",
            columnspan=2, pady=(8, 0)
        )
        self._build_active_alarms_panel(alarm_group)

        _, traffic_group = self._build_section_shell(
            page, 2, 0, "Communication Log",
            "Raw serial traffic and protocol-side events.",
            columnspan=2, pady=(8, 0)
        )
        traffic_group.grid_columnconfigure(0, weight=1)
        traffic_group.grid_rowconfigure(0, weight=1)

        traffic = tk.LabelFrame(traffic_group, text="Traffic Log", font=("Segoe UI", 9),
                                padx=8, pady=4, bg="#FFFFFF")
        traffic.grid(row=0, column=0, sticky="nsew", padx=10, pady=10)
        self._build_traffic_log(traffic)

    def _build_bms_monitor_panel(self, parent, column: int = 0):
        """BMS information panel for the monitor page."""
        _, panel = self._build_section_shell(
            parent, 0, column, "BMS Monitor",
            "Latest BMS snapshot and derived battery metrics."
        )
        panel.grid_columnconfigure(0, weight=1)
        panel.grid_rowconfigure(0, weight=1)
        panel.grid_rowconfigure(1, weight=1)
        panel.grid_rowconfigure(2, weight=1)
        panel.grid_rowconfigure(3, weight=0)

        self._build_charge_process_panel(panel)

        top_group = tk.LabelFrame(panel, text="BMS Snapshot", font=("Segoe UI", 9, "bold"),
                                  padx=8, pady=6, bg="#FFFFFF")
        top_group.grid(row=1, column=0, sticky="nsew", pady=(0, 8), padx=10)
        top_group.grid_columnconfigure(0, weight=1)
        top_group.grid_rowconfigure(0, weight=1)

        top = tk.Frame(top_group, bg="#FFFFFF")
        top.grid(row=0, column=0, sticky="nsew")
        for col in range(2):
            top.grid_columnconfigure(col, weight=1, uniform="bms_top")
        top.grid_rowconfigure(0, weight=1)
        top.grid_rowconfigure(1, weight=1)

        self._build_bms_overview_card(top, 0, columnspan=2, row=0)
        self._build_bms_request_card(top, 0, row=1)
        self._build_bms_relay_card(top, 1, row=1)

        bottom_group = tk.LabelFrame(panel, text="BMS Metrics", font=("Segoe UI", 9, "bold"),
                                     padx=8, pady=6, bg="#FFFFFF")
        bottom_group.grid(row=2, column=0, sticky="nsew", pady=(0, 8), padx=10)
        bottom_group.grid_columnconfigure(0, weight=1)
        bottom_group.grid_rowconfigure(0, weight=1)

        bottom = tk.Frame(bottom_group, bg="#FFFFFF")
        bottom.grid(row=0, column=0, sticky="nsew")
        for col in range(3):
            bottom.grid_columnconfigure(col, weight=1, uniform="bms_bottom")
        bottom.grid_rowconfigure(0, weight=1)

        self._build_bms_pack_card(bottom, 0)
        self._build_bms_cell_card(bottom, 1)
        self._build_bms_health_card(bottom, 2)

        footer = tk.Frame(panel, bg="#F0F0F0")
        footer.grid(row=3, column=0, sticky="ew", padx=10, pady=(0, 10))
        msg = (
            "Charge Process separates the live charging picture into three layers: "
            "BMS limits, controller targets, and actual charger output."
        )
        tk.Label(footer, text=msg, font=("Segoe UI", 9), bg="#F0F0F0",
                 justify=tk.LEFT, anchor="w", wraplength=420).pack(fill=tk.X)

    def _build_active_alarms_panel(self, parent):
        parent.grid_columnconfigure(0, weight=1, uniform="alarm_cols")
        parent.grid_columnconfigure(1, weight=1, uniform="alarm_cols")
        parent.grid_rowconfigure(0, weight=1)

        charger = tk.Frame(parent, bg="#FFFFFF")
        charger.grid(row=0, column=0, sticky="nsew", padx=(10, 6), pady=10)
        charger.grid_columnconfigure(0, weight=1)
        self._build_subsection_title(charger, "Charger").grid(row=0, column=0, sticky="w", pady=(0, 6))
        charger_list = tk.Frame(charger, bg="#FFFFFF")
        charger_list.grid(row=1, column=0, sticky="nsew")
        self._build_alarm_details(charger_list)

        bms = tk.Frame(parent, bg="#FFFFFF")
        bms.grid(row=0, column=1, sticky="nsew", padx=(6, 10), pady=10)
        bms.grid_columnconfigure(0, weight=1)
        self._build_subsection_title(bms, "BMS").grid(row=0, column=0, sticky="w", pady=(0, 6))
        bms_list = tk.Frame(bms, bg="#FFFFFF")
        bms_list.grid(row=1, column=0, sticky="nsew")
        self._build_bms_alarm_details(bms_list)

    def _build_monitor_module_list(self, parent):
        frame = tk.Frame(parent, bg="#F0F0F0")
        frame.grid(row=0, column=0, sticky="ew")
        frame.grid_columnconfigure(0, weight=1)

        tk.Label(frame, text="Observed Modules", font=("Segoe UI", 9, "bold"),
                 bg="#F0F0F0", anchor="w").grid(row=0, column=0, sticky="w")
        hint = tk.Label(frame, text="Select a module to inspect detailed charger telemetry.",
                        font=("Segoe UI", 8), fg="#666666", bg="#F0F0F0", anchor="w")
        hint.grid(row=1, column=0, sticky="w", pady=(2, 6))

        columns = ("addr", "driver", "online")
        self.tree_monitor_modules = ttk.Treeview(frame, columns=columns, show="headings", height=5)
        self.tree_monitor_modules.heading("addr", text="Addr")
        self.tree_monitor_modules.heading("driver", text="Driver")
        self.tree_monitor_modules.heading("online", text="Online")
        self.tree_monitor_modules.column("addr", width=70, anchor="center")
        self.tree_monitor_modules.column("driver", width=90, anchor="center")
        self.tree_monitor_modules.column("online", width=70, anchor="center")
        self.tree_monitor_modules.grid(row=2, column=0, sticky="ew")
        self.tree_monitor_modules.bind("<<TreeviewSelect>>", self._on_module_select)

    def _build_control_focus_panel(self, parent):
        parent.grid_columnconfigure(0, weight=1)

        tk.Label(parent, text="Review live values before applying write commands.",
                 font=("Segoe UI", 9), fg="#666666", bg="#FFFFFF", anchor="w").grid(
            row=0, column=0, sticky="ew", padx=14, pady=(12, 10)
        )

        identity = tk.LabelFrame(parent, text="State and Identity", font=("Segoe UI", 9, "bold"),
                                 padx=12, pady=10, bg="#FFFFFF")
        identity.grid(row=1, column=0, sticky="ew", padx=14, pady=(0, 10))
        identity.grid_columnconfigure(0, weight=1)
        self._add_focus_metric_row(identity, "Driver", self._ensure_detail_var("Driver"))
        self._add_focus_metric_row(identity, "State", self._ensure_detail_var("State"))
        self._add_focus_metric_row(identity, "Online", self._ensure_detail_var("Online"))
        self._add_focus_metric_row(identity, "Running", self._ensure_detail_var("Running"))

        output = tk.LabelFrame(parent, text="Electrical Output", font=("Segoe UI", 9, "bold"),
                               padx=12, pady=10, bg="#FFFFFF")
        output.grid(row=2, column=0, sticky="ew", padx=14, pady=(0, 10))
        output.grid_columnconfigure(0, weight=1)
        self._add_focus_metric_row(output, "Voltage", self._ensure_detail_var("Voltage"), "V")
        self._add_focus_metric_row(output, "Current", self._ensure_detail_var("Current"), "A")
        self._add_focus_metric_row(output, "Current Limit", self._ensure_detail_var("Curr Limit"), "A")

        alarms = tk.LabelFrame(parent, text="Alarm Status", font=("Segoe UI", 9, "bold"),
                               padx=12, pady=10, bg="#FFFFFF")
        alarms.grid(row=3, column=0, sticky="ew", padx=14, pady=(0, 14))
        alarms.grid_columnconfigure(0, weight=1)
        self._add_focus_metric_row(alarms, "Summary", self._ensure_detail_var("Alarm Summary"), value_wrap=540)

    def _add_focus_metric_row(self, parent, label: str, var: tk.StringVar, unit: str = "", value_wrap: int = 0):
        row = tk.Frame(parent, bg="#FFFFFF")
        row.pack(fill=tk.X, pady=(0, 8))
        row.grid_columnconfigure(0, minsize=150)
        row.grid_columnconfigure(1, weight=1)
        row.grid_columnconfigure(2, minsize=28)

        tk.Label(row, text=label, font=("Segoe UI", 10, "bold"), fg="#355C7D", bg="#FFFFFF",
                 anchor="w").grid(row=0, column=0, sticky="nw")
        tk.Label(row, textvariable=var, font=("Segoe UI", 13, "bold"), bg="#FFFFFF",
                 anchor="w", justify=tk.LEFT, wraplength=value_wrap).grid(row=0, column=1, sticky="ew")
        if unit:
            tk.Label(row, text=unit, font=("Segoe UI", 10), fg="#666666", bg="#FFFFFF",
                     anchor="w").grid(row=0, column=2, sticky="w", padx=(8, 0))

    def _build_control_notes(self, parent):
        tk.Label(parent, text="Keep command-side tasks simple and predictable.",
                 font=("Segoe UI", 8), fg="#666666", bg="#F0F0F0", anchor="w",
                 justify=tk.LEFT, wraplength=280).pack(fill=tk.X, pady=(0, 8))
        tips = [
            "Pick the target module first so Set V, Set I, START, and STOP affect the intended charger.",
            "Use Monitor to verify actual voltage, current, and alarm responses after each control action.",
            "Use Charge Config only for MCU charge-curve parameters, protection, and hardware limits.",
            "Emergency Stop is global and should remain available even when no module snapshot is selected.",
        ]
        for idx, text in enumerate(tips, start=1):
            row = tk.Frame(parent, bg="#F0F0F0")
            row.pack(fill=tk.X, pady=(0, 6))
            tk.Label(row, text=f"{idx}.", font=("Segoe UI", 9, "bold"),
                     fg="#355C7D", bg="#F0F0F0", width=2, anchor="nw").pack(side=tk.LEFT)
            tk.Label(row, text=text, font=("Segoe UI", 9), bg="#F0F0F0",
                     justify=tk.LEFT, anchor="w", wraplength=280).pack(side=tk.LEFT, fill=tk.X, expand=True)

    def _build_subsection_title(self, parent, text: str):
        return tk.Label(parent, text=text, font=("Segoe UI", 8, "bold"),
                        fg="#355C7D", bg="#F0F0F0", anchor="w")

    def _ensure_detail_var(self, key: str, default: str = "---") -> tk.StringVar:
        if key not in self.detail_vars:
            self.detail_vars[key] = tk.StringVar(value=default)
        return self.detail_vars[key]

    def _ensure_stats_var(self, key: str, default: str = "0") -> tk.StringVar:
        if key not in self.stats_vars:
            self.stats_vars[key] = tk.StringVar(value=default)
        return self.stats_vars[key]

    def _make_cfg_var(self, key: str, default: str = "0") -> tk.StringVar:
        if key in self.cfg_vars:
            return self.cfg_vars[key]
        var = tk.StringVar(value=default)
        self.cfg_vars[key] = var
        return var

    def _make_cfg_check(self, key: str, default: bool = False) -> tk.BooleanVar:
        if key in self.cfg_checks:
            return self.cfg_checks[key]
        var = tk.BooleanVar(value=default)
        self.cfg_checks[key] = var
        return var

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
        if unit:
            tk.Label(frame, text=unit, font=("Segoe UI", 8), fg="gray", bg="#F0F0F0", anchor="w").grid(row=0, column=2, sticky="w", padx=(6, 0))

    def _add_cfg_check(self, parent, row: int, column: int, label: str, key: str):
        frame = tk.Frame(parent, bg="#F0F0F0")
        frame.grid(row=row, column=column, sticky="w", padx=6, pady=4)
        tk.Label(frame, text=label, font=("Segoe UI", 9), bg="#F0F0F0").pack(side=tk.LEFT)
        tk.Checkbutton(frame, variable=self._make_cfg_check(key), bg="#F0F0F0", activebackground="#F0F0F0").pack(side=tk.LEFT, padx=(8, 0))

    def _set_cfg_section_enabled(self, widget, enabled: bool, keep_widgets: Optional[set] = None):
        keep_widgets = keep_widgets or set()
        disabled_bg = "#E8E8E8"
        disabled_fg = "#8A8A8A"
        normal_bg = "#F0F0F0"
        normal_fg = "#000000"

        def walk(node):
            preserve = node in keep_widgets
            try:
                if isinstance(node, ttk.Combobox):
                    if not preserve:
                        node.configure(state="readonly" if enabled else "disabled")
                elif isinstance(node, ttk.Entry):
                    if not preserve:
                        node.configure(state="normal" if enabled else "disabled")
                elif isinstance(node, tk.Checkbutton):
                    if not preserve:
                        node.configure(state="normal" if enabled else "disabled")
                    node.configure(
                        bg=normal_bg if enabled else disabled_bg,
                        activebackground=normal_bg if enabled else disabled_bg,
                        fg=normal_fg if enabled else disabled_fg,
                    )
                elif isinstance(node, (tk.Frame, tk.LabelFrame, tk.Label)):
                    if "bg" in node.keys():
                        node.configure(bg=normal_bg if enabled else disabled_bg)
                    if isinstance(node, tk.Label) and "fg" in node.keys():
                        node.configure(fg=normal_fg if enabled else disabled_fg)
            except tk.TclError:
                pass

            for child in node.winfo_children():
                walk(child)

        walk(widget)

    def _apply_charge_source_mode_ui(self):
        if not hasattr(self, "cfg_charge_source_mode"):
            return

        charge_source_name = self.cfg_charge_source_mode.get()
        standalone = charge_source_name == CHARGE_SOURCE_MODE_NAMES.get(1)

        if hasattr(self, "cfg_general_group"):
            self._set_cfg_section_enabled(self.cfg_general_group, True)
        if hasattr(self, "cfg_pack_params_group"):
            self._set_cfg_section_enabled(self.cfg_pack_params_group, True)
        if hasattr(self, "cfg_charge_window_group"):
            self._set_cfg_section_enabled(self.cfg_charge_window_group, not standalone)
        if hasattr(self, "cfg_control_group"):
            self._set_cfg_section_enabled(self.cfg_control_group, not standalone)
        if hasattr(self, "cfg_system_group"):
            self._set_cfg_section_enabled(
                self.cfg_system_group,
                not standalone,
                keep_widgets={self.cfg_charge_source_mode},
            )
        if hasattr(self, "cfg_bms_can_id_entry"):
            self.cfg_bms_can_id_entry.configure(state="normal" if not standalone else "disabled")
        if hasattr(self, "cfg_jack_charge_group"):
            self._set_cfg_section_enabled(self.cfg_jack_charge_group, not standalone)
        if hasattr(self, "cfg_jack_temp_group"):
            self._set_cfg_section_enabled(self.cfg_jack_temp_group, True)
        if hasattr(self, "cfg_module_group"):
            self._set_cfg_section_enabled(self.cfg_module_group, True)

    def _build_stage_panel(self, parent, title: str, enabled_key: str, delta_key: str,
                           threshold_keys: list[str], current_keys: list[str],
                           threshold_unit: str, current_unit: str,
                           threshold_label_prefix: str, current_label_prefix: str = "Current"):
        panel = tk.LabelFrame(parent, text=title, font=("Segoe UI", 8, "bold"),
                              padx=10, pady=8, bg="#F0F0F0")
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
            tk.Label(grid, text=f"{threshold_label_prefix} {idx + 1}", font=("Segoe UI", 8, "bold"),
                     bg="#F0F0F0", fg="#355C7D").grid(row=0, column=idx + 1, padx=4, pady=(0, 6))

        tk.Label(grid, text="Thresholds", font=("Segoe UI", 8), bg="#F0F0F0",
                 anchor="w").grid(row=1, column=0, sticky="w", padx=4, pady=4)
        for idx, key in enumerate(threshold_keys):
            ttk.Entry(grid, textvariable=self._make_cfg_var(key), width=9).grid(
                row=1, column=idx + 1, padx=4, pady=4, sticky="ew"
            )

        tk.Label(grid, text="", bg="#F0F0F0").grid(row=2, column=0, padx=4, pady=(8, 4))
        for idx, key in enumerate(current_keys):
            tk.Label(grid, text=f"{current_label_prefix} {idx + 1}-{idx + 2}", font=("Segoe UI", 8, "bold"),
                     bg="#F0F0F0", fg="#6C5B7B").grid(row=2, column=idx + 1, padx=4, pady=(8, 4))
        tk.Label(grid, text="Current Limits", font=("Segoe UI", 8), bg="#F0F0F0",
                 anchor="w").grid(row=3, column=0, sticky="w", padx=4, pady=4)
        for idx, key in enumerate(current_keys):
            ttk.Entry(grid, textvariable=self._make_cfg_var(key), width=9).grid(
                row=3, column=idx + 1, padx=4, pady=4, sticky="ew"
            )

        units = tk.Frame(panel, bg="#F0F0F0")
        units.pack(fill=tk.X, pady=(6, 0))
        units_right = tk.Frame(units, bg="#F0F0F0")
        units_right.pack(side=tk.RIGHT)
        tk.Label(units_right, text=f"Threshold unit: {threshold_unit}", font=("Segoe UI", 8),
                 fg="#666666", bg="#F0F0F0", anchor="e", justify=tk.RIGHT).pack(anchor="e")
        tk.Label(units_right, text=f"Current unit: {current_unit}", font=("Segoe UI", 8),
                 fg="#666666", bg="#F0F0F0", anchor="e", justify=tk.RIGHT).pack(anchor="e")

    def _build_charge_config_tab(self, parent):
        parent.grid_columnconfigure(0, weight=1)
        parent.grid_rowconfigure(0, weight=1)

        outer = tk.Frame(parent, bg="#F0F0F0")
        outer.grid(row=0, column=0, sticky="nsew")
        outer.grid_columnconfigure(0, weight=1)
        outer.grid_rowconfigure(1, weight=1)

        toolbar = tk.Frame(outer, bg="#F0F0F0")
        toolbar.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        tk.Label(toolbar, text="Charge Cycle Configuration", font=("Segoe UI", 10, "bold"), bg="#F0F0F0").pack(side=tk.LEFT)

        # Status label — shows result of Read/Write MCU operations inline
        self._cfg_status_var = tk.StringVar(value="")
        tk.Label(toolbar, textvariable=self._cfg_status_var, font=("Segoe UI", 9),
                 bg="#F0F0F0", fg="#0066CC").pack(side=tk.LEFT, padx=(12, 0))

        # Right side buttons
        btn_frame = tk.Frame(toolbar, bg="#F0F0F0")
        btn_frame.pack(side=tk.RIGHT)
        ttk.Button(btn_frame, text="Import", command=self._import_charge_config, width=9).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(btn_frame, text="Export", command=self._export_charge_config, width=9).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(btn_frame, text="Read MCU", command=self._request_charge_config, width=9).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(btn_frame, text="Write MCU", command=self._send_charge_config, width=9).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(btn_frame, text="Defaults", command=self._load_charge_config_defaults, width=9).pack(side=tk.LEFT)

        self._charge_cfg_canvas = tk.Canvas(outer, bg="#F0F0F0", highlightthickness=0)
        canvas = self._charge_cfg_canvas
        canvas.grid(row=1, column=0, sticky="nsew")
        scrollbar = ttk.Scrollbar(outer, orient="vertical", command=canvas.yview)
        scrollbar.grid(row=1, column=1, sticky="ns")
        h_scrollbar = ttk.Scrollbar(outer, orient="horizontal", command=canvas.xview)
        h_scrollbar.grid(row=2, column=0, sticky="ew")
        canvas.configure(yscrollcommand=scrollbar.set, xscrollcommand=h_scrollbar.set)

        body = tk.Frame(canvas, bg="#F0F0F0")
        self.charge_cfg_body = body

        # after_idle-based debounce (Fix 3: Canvas Configure cascade)
        def _cfg_sync_scrollregion(_event=None):
            def do_sync():
                canvas.configure(scrollregion=canvas.bbox("all"))
                canvas._sync_scheduled = False
            if not getattr(canvas, '_sync_scheduled', False):
                canvas._sync_scheduled = True
                canvas.after_idle(do_sync)

        body_window = canvas.create_window((0, 0), window=body, anchor="nw")

        # Keep the configuration split balanced for wide and responsive views:
        # charge controls 4 / protection and hardware 3.
        body.grid_columnconfigure(0, weight=4, uniform="cfg_cols")
        body.grid_columnconfigure(1, weight=3, uniform="cfg_cols")
        config_min_width = self._responsive_min_width(1120)

        def _cfg_sync_width(event):
            target_w = max(event.width, config_min_width)
            current_w = canvas.itemconfigure(body_window, 'width')
            if isinstance(current_w, dict):
                current_w = current_w.get('value', target_w)
            if current_w != target_w:
                canvas.itemconfigure(body_window, width=target_w)
            _cfg_sync_scrollregion()

        body.bind("<Configure>", _cfg_sync_scrollregion)
        canvas.bind("<Configure>", _cfg_sync_width)

        # MouseWheel support
        self._bind_mousewheel_to_canvas(canvas, body)

        self._build_charge_general_group(body)
        self._build_charge_control_group(body)
        self._build_charge_protection_group(body)

        footer = tk.LabelFrame(body, text="Notes", font=("Segoe UI", 9), padx=8, pady=6, bg="#F0F0F0")
        footer.grid(row=2, column=0, columnspan=2, sticky="ew", padx=4, pady=(8, 0))
        note = (
            "This tab uses a dedicated debug payload to read and write the charge cycle configuration on the MCU. "
            "The firmware currently keeps this configuration in RAM only and does not persist it to flash yet."
        )
        tk.Label(footer, text=note, font=("Segoe UI", 9), bg="#F0F0F0",
                 justify=tk.LEFT, anchor="w", wraplength=1080).pack(fill=tk.X)

        self._load_charge_config_defaults()

    def _build_charge_general_group(self, parent):
        group = tk.LabelFrame(parent, text="General Limits", font=("Segoe UI", 9, "bold"), padx=10, pady=8, bg="#F0F0F0")
        self.cfg_general_group = group
        group.grid(row=0, column=0, sticky="nsew", padx=(4, 6), pady=(0, 8))
        group.grid_columnconfigure(0, weight=1)
        group.grid_columnconfigure(1, weight=1)

        summary = tk.Frame(group, bg="#EAF2F8", bd=1, relief="solid")
        summary.grid(row=0, column=0, columnspan=2, sticky="ew", padx=4, pady=(0, 10))
        tk.Label(summary, text="Pack-level constraints and charge window",
                 font=("Segoe UI", 10, "bold"), fg="#1F3A5F", bg="#EAF2F8").pack(anchor="w", padx=10, pady=(8, 2))
        tk.Label(summary, text="Use these values to define the global operating envelope before stage tuning.",
                 font=("Segoe UI", 8), fg="#4F5D75", bg="#EAF2F8").pack(anchor="w", padx=10, pady=(0, 8))

        left = tk.LabelFrame(group, text="Pack Parameters", font=("Segoe UI", 8, "bold"),
                             padx=8, pady=8, bg="#F0F0F0")
        self.cfg_pack_params_group = left
        left.grid(row=1, column=0, sticky="nsew", padx=(4, 6))
        left.grid_columnconfigure(0, weight=1)
        left.grid_columnconfigure(1, weight=1)
        self._add_cfg_entry(left, 0, 0, "Battery Capacity", "battery_capacity_ah", "Ah")
        self._add_cfg_entry(left, 0, 1, "Temperature Limit", "temp_limit_c", "C")
        self._add_cfg_entry(left, 1, 0, "V Min", "vmin_v", "V")
        self._add_cfg_entry(left, 1, 1, "V Max", "vmax_v", "V")
        self._add_cfg_entry(left, 2, 0, "I Min", "imin_c", "C")
        self._add_cfg_entry(left, 2, 1, "I Max", "imax_c", "C")

        right = tk.LabelFrame(group, text="Charge Window", font=("Segoe UI", 8, "bold"),
                              padx=8, pady=8, bg="#F0F0F0")
        self.cfg_charge_window_group = right
        right.grid(row=1, column=1, sticky="nsew", padx=(6, 4))
        right.grid_columnconfigure(0, weight=1)
        right.grid_columnconfigure(1, weight=1)
        self._add_cfg_entry(right, 0, 0, "V Precharge", "vpre_v", "V")
        self._add_cfg_entry(right, 0, 1, "V Low", "vlow_v", "V")
        self._add_cfg_entry(right, 1, 0, "I Precharge", "ipre_c", "C")
        self._add_cfg_entry(right, 1, 1, "I Low", "ilow_c", "C")

    def _build_charge_control_group(self, parent):
        group = tk.LabelFrame(parent, text="Charging Strategy", font=("Segoe UI", 9, "bold"), padx=10, pady=8, bg="#F0F0F0")
        self.cfg_control_group = group
        group.grid(row=1, column=0, sticky="nsew", padx=(4, 6), pady=(0, 8))

        header = tk.Frame(group, bg="#F0F0F0")
        header.pack(fill=tk.X, pady=(0, 10))
        tk.Label(header, text="Five-threshold control curves", font=("Segoe UI", 9, "bold"),
                 fg="#355C7D", bg="#F0F0F0").pack(side=tk.LEFT)
        tk.Label(header, text="T1..T5 define thresholds, Current 1..4 define the output between thresholds.",
                 font=("Segoe UI", 8), fg="#666666", bg="#F0F0F0").pack(side=tk.RIGHT)

        self._build_stage_panel(
            group,
            "Cell Voltage Stages",
            "cell_volt_enabled",
            "cell_volt_delta_v",
            ["cell_volt_1_v", "cell_volt_2_v", "cell_volt_3_v", "cell_volt_4_v", "cell_volt_5_v"],
            ["cell_curr_1_c", "cell_curr_2_c", "cell_curr_3_c", "cell_curr_4_c"],
            "V", "C",
            "Cell Voltage",
            "Current"
        )
        self._build_stage_panel(
            group,
            "Temperature Stages",
            "temp_enabled",
            "temp_delta_c",
            ["temp_1_c", "temp_2_c", "temp_3_c", "temp_4_c", "temp_5_c"],
            ["temp_curr_1_c", "temp_curr_2_c", "temp_curr_3_c", "temp_curr_4_c"],
            "C", "C",
            "Temperature",
            "Current"
        )
        self._build_stage_panel(
            group,
            "SOC Stages",
            "soc_enabled",
            "soc_delta_pct",
            ["soc_1_pct", "soc_2_pct", "soc_3_pct", "soc_4_pct", "soc_5_pct"],
            ["soc_curr_1_c", "soc_curr_2_c", "soc_curr_3_c", "soc_curr_4_c"],
            "%", "C",
            "SOC",
            "Current"
        )

    def _build_charge_protection_group(self, parent):
        group = tk.LabelFrame(parent, text="Protection and Hardware", font=("Segoe UI", 9, "bold"), padx=10, pady=8, bg="#F0F0F0")
        group.grid(row=0, column=1, rowspan=2, sticky="nsew", padx=(6, 4), pady=(0, 8))
        group.grid_columnconfigure(0, weight=1)

        hero = tk.Frame(group, bg="#F7F3E9", bd=1, relief="solid")
        hero.grid(row=0, column=0, sticky="ew", pady=(0, 10))
        tk.Label(hero, text="Protection, mapping, and charger envelope",
                 font=("Segoe UI", 10, "bold"), fg="#6A4C1D", bg="#F7F3E9").pack(anchor="w", padx=10, pady=(8, 2))
        tk.Label(hero, text="These settings define safety cutoffs, system addressing, and the physical charger limits.",
                 font=("Segoe UI", 8), fg="#7A6A58", bg="#F7F3E9").pack(anchor="w", padx=10, pady=(0, 8))

        system = tk.LabelFrame(group, text="System Mapping", font=("Segoe UI", 8, "bold"),
                               padx=8, pady=8, bg="#F0F0F0")
        self.cfg_system_group = system
        system.grid(row=1, column=0, sticky="ew", pady=(0, 10))
        system.grid_columnconfigure(0, weight=1)
        system.grid_columnconfigure(1, weight=1)
        self._add_cfg_entry(system, 0, 0, "BMS CAN ID", "can_battery_id")

        source_row = tk.Frame(system, bg="#F0F0F0")
        source_row.grid(row=0, column=1, sticky="ew", padx=6, pady=4)
        tk.Label(source_row, text="Charge Source", font=("Segoe UI", 9), bg="#F0F0F0").pack(side=tk.LEFT)
        self.cfg_charge_source_mode = ttk.Combobox(
            source_row,
            state="readonly",
            width=24,
            values=[CHARGE_SOURCE_MODE_NAMES[idx] for idx in sorted(CHARGE_SOURCE_MODE_NAMES)]
        )
        self.cfg_charge_source_mode.pack(side=tk.LEFT, padx=(8, 0))
        self.cfg_charge_source_mode.current(0)
        self.cfg_charge_source_mode.bind("<<ComboboxSelected>>", lambda _event: self._apply_charge_source_mode_ui())

        jack_charge = tk.LabelFrame(group, text="Charge Jack Protection", font=("Segoe UI", 8, "bold"),
                                    padx=8, pady=8, bg="#F0F0F0")
        self.cfg_jack_charge_group = jack_charge
        jack_charge.grid(row=2, column=0, sticky="ew", pady=(0, 10))
        jack_charge.grid_columnconfigure(0, weight=1)
        jack_charge.grid_columnconfigure(1, weight=1)
        jack_charge.grid_columnconfigure(2, weight=1)
        self._add_cfg_check(jack_charge, 0, 0, "Enabled", "protect_jack_charge_enabled")
        self._add_cfg_entry(jack_charge, 0, 1, "Delta V", "protect_jack_charge_delta_v", "V")
        self._add_cfg_entry(jack_charge, 0, 2, "Delay", "protect_jack_charge_delay_s", "s")

        jack_temp = tk.LabelFrame(group, text="Jack Temperature Protection", font=("Segoe UI", 8, "bold"),
                                  padx=8, pady=8, bg="#F0F0F0")
        self.cfg_jack_temp_group = jack_temp
        jack_temp.grid(row=3, column=0, sticky="ew", pady=(0, 10))
        for col in range(4):
            jack_temp.grid_columnconfigure(col, weight=1)
        self._add_cfg_check(jack_temp, 0, 0, "Enabled", "protect_jack_temp_enabled")
        self._add_cfg_entry(jack_temp, 0, 1, "Threshold", "protect_jack_temp_threshold_c", "C")
        self._add_cfg_entry(jack_temp, 0, 2, "Delta", "protect_jack_temp_delta_c", "C")
        self._add_cfg_entry(jack_temp, 0, 3, "Delay", "protect_jack_temp_delay_s", "s")
        self._add_cfg_entry(jack_temp, 0, 4, "Power Limit", "protect_jack_temp_power_limit_pct", "%")

        module = tk.LabelFrame(group, text="Charger Module Envelope", font=("Segoe UI", 8, "bold"),
                               padx=8, pady=8, bg="#F0F0F0")
        self.cfg_module_group = module
        module.grid(row=4, column=0, sticky="ew")
        module.grid_columnconfigure(0, weight=1)
        module.grid_columnconfigure(1, weight=1)
        type_row = tk.Frame(module, bg="#F0F0F0")
        type_row.grid(row=0, column=0, columnspan=2, sticky="ew", padx=6, pady=(0, 6))
        tk.Label(type_row, text="Module Type", font=("Segoe UI", 9), bg="#F0F0F0").pack(side=tk.LEFT)
        self.cfg_module_type = ttk.Combobox(type_row, state="readonly", width=24,
                                            values=[MODULE_TYPE_NAMES[idx] for idx in sorted(MODULE_TYPE_NAMES)])
        self.cfg_module_type.pack(side=tk.LEFT, padx=(8, 0))
        self.cfg_module_type.current(1)
        self._add_cfg_entry(module, 1, 0, "Source Modules", "source_module_count")
        self._add_cfg_entry(module, 2, 0, "U Min", "module_u_min_v", "V")
        self._add_cfg_entry(module, 2, 1, "U Max", "module_u_max_v", "V")
        self._add_cfg_entry(module, 3, 0, "I Min", "module_i_min_a", "A")
        self._add_cfg_entry(module, 3, 1, "I Max", "module_i_max_a", "A")

    def _build_module_detail(self, parent):
        """Module Detail cards - matches layout exactly"""
        container = tk.Frame(parent, bg="#F0F0F0")
        container.pack(fill=tk.BOTH, expand=True)
        container.grid_columnconfigure(0, weight=1)
        container.grid_rowconfigure(0, weight=2)
        container.grid_rowconfigure(1, weight=1)
        container.grid_rowconfigure(2, weight=0)

        # Row 0: Basic is the primary area, full width
        row0 = tk.Frame(container, bg="#F0F0F0")
        row0.grid(row=0, column=0, sticky="nsew", pady=(0, 6))
        row0.grid_columnconfigure(0, weight=1)
        row0.grid_rowconfigure(0, weight=1)
        self._build_basic_card(row0, column=0)

        # Row 1: Secondary metrics
        row1 = tk.Frame(container, bg="#F0F0F0")
        row1.grid(row=1, column=0, sticky="nsew", pady=(0, 6))
        for col in range(4):
            row1.grid_columnconfigure(col, weight=1, uniform="detail_row2")
        row1.grid_rowconfigure(0, weight=1)
        self._build_temperatures_card(row1, column=0)
        self._build_ac_input_card(row1, column=1)
        self._build_power_card(row1, column=2)
        self._build_pfc_card(row1, column=3)

        # Row 2: Alarms Summary (full width)
        row2 = tk.Frame(container, bg="#F0F0F0")
        row2.grid(row=2, column=0, sticky="ew", pady=(0, 6))
        self._build_alarm_card(row2)

    def _build_card_frame(self, parent, title: str, column: Optional[int] = None,
                          padx: Optional[tuple[int, int]] = None,
                          columnspan: int = 1, row: int = 0) -> tk.Frame:
        """Create a card frame"""
        card = tk.LabelFrame(parent, text=title, font=("Segoe UI", 8),
                             padx=6, pady=4, bg="#F0F0F0")
        if column is None:
            card.pack(fill=tk.BOTH, expand=True)
        else:
            card.grid(row=row, column=column, columnspan=columnspan, sticky="nsew",
                      padx=padx if padx is not None else (0 if column == 0 else 6, 0))
        return card

    def _add_card_row(self, parent, label: str, var: tk.StringVar, unit: str = ""):
        """Add a row to a card"""
        row = tk.Frame(parent, bg="#F0F0F0")
        row.pack(fill=tk.X, pady=1)
        row.grid_columnconfigure(1, weight=1)
        tk.Label(row, text=f"{label}:", font=("Segoe UI", 9), bg="#F0F0F0",
                 width=12, anchor="w").grid(row=0, column=0, sticky="w")
        tk.Label(row, textvariable=var, font=("Segoe UI", 10, "bold"), bg="#F0F0F0",
                 anchor="w").grid(row=0, column=1, sticky="w")
        if unit:
            tk.Label(row, text=unit, font=("Segoe UI", 9), fg="gray", bg="#F0F0F0",
                     width=3, anchor="e").grid(row=0, column=2, sticky="e")

    def _add_bms_row(self, parent, label: str, var: tk.StringVar, unit: str = ""):
        """Add a row to a BMS card."""
        row = tk.Frame(parent, bg="#F0F0F0")
        row.pack(fill=tk.X, pady=2)
        row.grid_columnconfigure(0, weight=0, minsize=92)
        row.grid_columnconfigure(1, weight=1, minsize=0)
        tk.Label(row, text=f"{label}:", font=("Segoe UI", 9), bg="#F0F0F0",
                 width=12, anchor="nw", justify="left", wraplength=92).grid(
                     row=0, column=0, sticky="ew", padx=(0, 4))
        value_label = tk.Label(row, textvariable=var, font=("Segoe UI", 10, "bold"),
                               bg="#F0F0F0", anchor="w", justify="left", wraplength=150)
        value_label.grid(row=0, column=1, sticky="ew")
        if unit:
            tk.Label(row, text=unit, font=("Segoe UI", 9), fg="gray", bg="#F0F0F0",
                     width=4, anchor="e").grid(row=0, column=2, sticky="e")

    def _build_bms_info_card(self, parent, title: str, column: int, columnspan: int = 1, row: int = 0) -> tk.Frame:
        return self._build_card_frame(parent, title, column=column, columnspan=columnspan, row=row)

    def _make_bms_var(self, key: str, default: str = "---") -> tk.StringVar:
        var = tk.StringVar(value=default)
        self.bms_vars[key] = var
        return var

    def _make_process_var(self, key: str, default: str = "---") -> tk.StringVar:
        var = tk.StringVar(value=default)
        self.process_vars[key] = var
        return var

    def _build_process_row(self, parent, label: str, var: tk.StringVar, unit: str = ""):
        row = tk.Frame(parent, bg="#FFFFFF")
        row.pack(fill=tk.X, pady=2)
        row.grid_columnconfigure(0, weight=0, minsize=102)
        row.grid_columnconfigure(1, weight=1, minsize=0)
        tk.Label(row, text=f"{label}:", font=("Segoe UI", 9), bg="#FFFFFF",
                 width=13, anchor="nw", justify="left", wraplength=102).grid(
                     row=0, column=0, sticky="ew", padx=(0, 4))
        value_label = tk.Label(row, textvariable=var, font=("Segoe UI", 10, "bold"),
                               bg="#FFFFFF", anchor="w", justify="left", wraplength=120)
        value_label.grid(row=0, column=1, sticky="ew")
        if unit:
            tk.Label(row, text=unit, font=("Segoe UI", 9), fg="#666666", bg="#FFFFFF",
                     width=4, anchor="e").grid(row=0, column=2, sticky="e")

    def _build_charge_process_panel(self, parent):
        group = tk.LabelFrame(parent, text="Charge Process", font=("Segoe UI", 9, "bold"),
                              padx=8, pady=6, bg="#FFFFFF")
        group.grid(row=0, column=0, sticky="nsew", pady=(10, 8), padx=10)
        group.grid_columnconfigure(0, weight=1)

        summary = tk.Frame(group, bg="#FFFFFF")
        summary.pack(fill=tk.X, pady=(0, 8))
        summary.grid_columnconfigure(0, weight=1, uniform="process_summary")
        summary.grid_columnconfigure(1, weight=1, uniform="process_summary")

        left = tk.Frame(summary, bg="#FFFFFF")
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        right = tk.Frame(summary, bg="#FFFFFF")
        right.grid(row=0, column=1, sticky="nsew", padx=(8, 0))

        self._build_process_row(left, "Controller State", self._make_process_var("Controller State"))
        self._build_process_row(left, "Control Mode", self._make_process_var("Control Mode"))
        self._build_process_row(right, "Process Summary", self._make_process_var("Process Summary"))
        self._build_process_row(right, "Modules Online", self._make_process_var("Modules Online"))

        metrics = tk.Frame(group, bg="#FFFFFF")
        metrics.pack(fill=tk.BOTH, expand=True)
        for col in range(3):
            metrics.grid_columnconfigure(col, weight=1, uniform="process_cols")

        bms_card = tk.LabelFrame(metrics, text="BMS Limits", font=("Segoe UI", 9),
                                 padx=8, pady=6, bg="#FFFFFF")
        bms_card.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        self._build_process_row(bms_card, "BMS Status", self._make_process_var("BMS Status"))
        self._build_process_row(bms_card, "Voltage Target", self._make_process_var("BMS Voltage Target"), "V")
        self._build_process_row(bms_card, "Current Limit", self._make_process_var("BMS Current Limit"), "A")
        self._build_process_row(bms_card, "Battery Max Temp", self._make_process_var("Battery Max Temp"), "C")

        ctrl_card = tk.LabelFrame(metrics, text="Controller Target", font=("Segoe UI", 9),
                                  padx=8, pady=6, bg="#FFFFFF")
        ctrl_card.grid(row=0, column=1, sticky="nsew", padx=6)
        self._build_process_row(ctrl_card, "Voltage Target", self._make_process_var("Controller Voltage Target"), "V")
        self._build_process_row(ctrl_card, "Current Target", self._make_process_var("Controller Current Target"), "A")
        self._build_process_row(ctrl_card, "Charge Status", self._make_process_var("Charge Status"))
        self._build_process_row(ctrl_card, "Active Logic", self._make_process_var("Active Logic"))
        self._build_process_row(ctrl_card, "Charge Level", self._make_process_var("Charge Level"))
        self._build_process_row(ctrl_card, "Current Limit", self._make_process_var("Current Limit"), "C")
        self._build_process_row(ctrl_card, "Stop Reason", self._make_process_var("Stop Reason"))
        self._build_process_row(ctrl_card, "Controller Fault", self._make_process_var("Controller Fault"))

        actual_card = tk.LabelFrame(metrics, text="Actual Charger Output", font=("Segoe UI", 9),
                                    padx=8, pady=6, bg="#FFFFFF")
        actual_card.grid(row=0, column=2, sticky="nsew", padx=(6, 0))
        self._build_process_row(actual_card, "Charge Voltage", self._make_process_var("Actual Charge Voltage"), "V")
        self._build_process_row(actual_card, "Charge Current", self._make_process_var("Actual Charge Current"), "A")
        self._build_process_row(actual_card, "Charger Max Temp", self._make_process_var("Charger Max Temp"), "C")

    def _build_bms_overview_card(self, parent, column: int, columnspan: int = 1, row: int = 0):
        """Identity/connectivity only -- electrical values live in Battery
        Pack, SOC/SOH live in Health Summary. Kept single-owner per field
        so no _make_bms_var() key is ever created twice (see the
        debug_app BMS Monitor standardization plan for why that mattered:
        a second create silently orphans the first widget's StringVar)."""
        card = self._build_bms_info_card(parent, "BMS Overview", column, columnspan=columnspan, row=row)
        content = tk.Frame(card, bg="#F0F0F0")
        content.pack(fill=tk.BOTH, expand=True)

        for name, unit in (("State", ""), ("Online", ""), ("Last RX Tick", "ms")):
            self._add_bms_row(content, name, self._make_bms_var(name), unit)

    def _build_bms_pack_card(self, parent, column: int):
        card = self._build_bms_info_card(parent, "Battery Pack", column)
        rows = [("Batt Voltage", "V"), ("Batt Current", "A"), ("Cap Remain", "Ah"), ("Rate Capacity", "Ah")]
        for name, unit in rows:
            self._add_bms_row(card, name, self._make_bms_var(name), unit)

    def _build_bms_request_card(self, parent, column: int, row: int = 0):
        card = self._build_bms_info_card(parent, "Charge Request", column, row=row)
        rows = [("Req Voltage", "V"), ("Req Current", "A")]
        for name, unit in rows:
            self._add_bms_row(card, name, self._make_bms_var(name), unit)

    def _build_bms_cell_card(self, parent, column: int):
        card = self._build_bms_info_card(parent, "Cell Extremes", column)
        rows = [
            ("Max Cell Volt", "mV"),
            ("Min Cell Volt", "mV"),
            ("Max Cell Temp", "C"),
            ("Min Cell Temp", "C"),
        ]
        for name, unit in rows:
            self._add_bms_row(card, name, self._make_bms_var(name), unit)

    def _build_bms_relay_card(self, parent, column: int, row: int = 0):
        card = self._build_bms_info_card(parent, "Relay Status", column, row=row)
        for name in ("Charge Relay", "Discharge Relay", "Alarm Count"):
            self._add_bms_row(card, name, self._make_bms_var(name))

    def _build_bms_health_card(self, parent, column: int):
        card = self._build_bms_info_card(parent, "Health Summary", column)
        rows = [("SOC", "%"), ("SOH", "%"), ("Cell Delta", "mV"), ("Temp Delta", "C"), ("BMS Alarms", "")]
        for name, unit in rows:
            self._add_bms_row(card, name, self._make_bms_var(name), unit)

    def _build_bms_alarm_details(self, parent):
        container = tk.Frame(parent, bg="#F0F0F0")
        container.pack(fill=tk.BOTH, expand=True)

        header = tk.Frame(container, bg="#F0F0F0")
        header.pack(fill=tk.X)
        self.lbl_bms_alarm_state = tk.Label(header, text="No BMS alarms",
                                            font=("Segoe UI", 9), fg="green", bg="#F0F0F0")
        self.lbl_bms_alarm_state.pack(side=tk.LEFT)
        tk.Button(header, text="Read BMS", command=self._request_bms_snapshot,
                  font=("Segoe UI", 8), relief="flat").pack(side=tk.RIGHT)

        list_frame = tk.Frame(container, bg="white", relief="solid", bd=1)
        list_frame.pack(fill=tk.BOTH, expand=True, pady=(6, 0))
        self.lst_bms_alarms = tk.Listbox(list_frame, font=("Consolas", 9),
                                         bg="white", bd=0, highlightthickness=0)
        self.lst_bms_alarms.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar = ttk.Scrollbar(list_frame, orient="vertical", command=self.lst_bms_alarms.yview)
        self.lst_bms_alarms.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

    def _build_basic_card(self, parent, column: Optional[int] = None, columnspan: int = 1):
        """Basic info - 7 items"""
        card = self._build_card_frame(
            parent,
            "Basic",
            column=column,
            padx=(0, 0) if column is not None else None,
            columnspan=columnspan,
        )
        content = tk.Frame(card, bg="#F0F0F0")
        content.pack(fill=tk.BOTH, expand=True)
        content.grid_columnconfigure(0, weight=1, uniform="basic_cols")
        content.grid_columnconfigure(1, weight=1, uniform="basic_cols")
        content.grid_rowconfigure(0, weight=1)

        left_col = tk.Frame(content, bg="#F0F0F0")
        left_col.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        right_col = tk.Frame(content, bg="#F0F0F0")
        right_col.grid(row=0, column=1, sticky="nsew", padx=(8, 0))

        left_items = [("Driver", ""), ("State", ""), ("Online", ""), ("Running", "")]
        right_items = [("Voltage", "V"), ("Current", "A"), ("Curr Limit", "A")]

        for name, unit in left_items:
            var = self._ensure_detail_var(name)
            self._add_card_row(left_col, name, var, unit)

        for name, unit in right_items:
            var = self._ensure_detail_var(name)
            self._add_card_row(right_col, name, var, unit)

    def _build_temperatures_card(self, parent, column: int):
        """Temperatures card"""
        temp_card = self._build_card_frame(parent, "Temperatures", column=column)
        items = [("Temp DCDC", "C"), ("Temp Ambient", "C"), ("Temp PFC", "C")]
        for name, unit in items:
            var = self._ensure_detail_var(name)
            self._add_card_row(temp_card, name, var, unit)

    def _build_ac_input_card(self, parent, column: int):
        """AC input card"""
        ac_card = self._build_card_frame(parent, "AC Input", column=column)
        items = [("AC Phase A", "V"), ("AC Phase B", "V"), ("AC Phase C", "V")]
        for name, unit in items:
            var = self._ensure_detail_var(name)
            self._add_card_row(ac_card, name, var, unit)

    def _build_power_card(self, parent, column: int):
        """Power card"""
        power_card = self._build_card_frame(parent, "Power", column=column)
        items = [("Input Power", "W"), ("Rated Power", "W"), ("Rated Current", "A")]
        for name, unit in items:
            var = self._ensure_detail_var(name)
            self._add_card_row(power_card, name, var, unit)

    def _build_pfc_card(self, parent, column: int):
        """PFC bus card"""
        pfc_card = self._build_card_frame(parent, "PFC Bus", column=column)
        items = [("PFC Bus +", "V"), ("PFC Bus -", "V"), ("PFC Fault", "")]
        for name, unit in items:
            var = self._ensure_detail_var(name)
            self._add_card_row(pfc_card, name, var, unit)

    def _build_alarm_card(self, parent):
        """Alarm summary (full width)"""
        card = tk.LabelFrame(parent, text="Alarms", font=("Segoe UI", 8),
                             padx=6, pady=4, bg="#F0F0F0")
        card.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        # Alarm Summary row
        row = tk.Frame(card, bg="#F0F0F0")
        row.pack(fill=tk.X)
        tk.Label(row, text="Summary:", font=("Segoe UI", 9), bg="#F0F0F0",
                 width=12, anchor="w").pack(side=tk.LEFT)
        tk.Label(row, textvariable=self._ensure_detail_var("Alarm Summary", "None"), font=("Segoe UI", 10, "bold"),
                 bg="#F0F0F0", anchor="w").pack(side=tk.LEFT, fill=tk.X, expand=True)

    def _build_compact_debug_stats(self, parent):
        """Compact comm stats for lower-left debug area."""
        card = tk.LabelFrame(parent, text="Debug Stats", font=("Segoe UI", 8),
                             padx=6, pady=6, bg="#F0F0F0")
        card.pack(side=tk.BOTTOM, fill=tk.X, pady=(12, 0))

        content = tk.Frame(card, bg="#F0F0F0")
        content.pack(fill=tk.X)
        content.grid_columnconfigure(0, weight=1, uniform="compact_stats")
        content.grid_columnconfigure(1, weight=1, uniform="compact_stats")

        stats = ["TX Count", "RX Count", "Error Count", "Timeout Count", "Recovery Count"]
        for i, stat in enumerate(stats):
            item = tk.Frame(content, bg="#F0F0F0")
            item.grid(row=i // 2, column=i % 2, sticky="ew",
                      padx=(0 if i % 2 == 0 else 8, 0), pady=(0, 4))
            tk.Label(item, text=stat, font=("Segoe UI", 7), fg="gray",
                     bg="#F0F0F0", anchor="w").pack(side=tk.LEFT)
            var = self._ensure_stats_var(stat)
            tk.Label(item, textvariable=var, font=("Segoe UI", 9, "bold"),
                     bg="#F0F0F0", anchor="e").pack(side=tk.RIGHT)

    def _build_comm_stats(self, parent, column: Optional[int] = None):
        """Comm Stats card"""
        card = tk.LabelFrame(parent, text="Comm Stats", font=("Segoe UI", 8),
                             padx=6, pady=4, bg="#F0F0F0")
        if column is None:
            card.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        else:
            card.grid(row=0, column=column, sticky="nsew", padx=(6, 0))

        content = tk.Frame(card, bg="#F0F0F0")
        content.pack(fill=tk.BOTH, expand=True)
        content.grid_columnconfigure(0, weight=1, uniform="stats_cols")
        content.grid_columnconfigure(1, weight=1, uniform="stats_cols")
        content.grid_rowconfigure(0, weight=1)
        content.grid_rowconfigure(1, weight=1)
        content.grid_rowconfigure(2, weight=1)

        stats = ["TX Count", "RX Count", "Error Count", "Timeout Count", "Recovery Count"]
        for i, stat in enumerate(stats):
            col = tk.Frame(content, bg="#F0F0F0")
            row_idx = i // 2
            col_idx = i % 2
            if i == 4:
                row_idx = 2
                col_idx = 0
            col.grid(row=row_idx, column=col_idx, sticky="nsew",
                     padx=(0 if col_idx == 0 else 8, 0), pady=(0, 4))
            tk.Label(col, text=stat, font=("Segoe UI", 7), fg="gray",
                     bg="#F0F0F0").pack()
            var = self._ensure_stats_var(stat)
            tk.Label(col, textvariable=var, font=("Segoe UI", 11, "bold"),
                     bg="#F0F0F0").pack()

    def _build_alarm_details(self, parent):
        """Alarm Details panel"""
        container = tk.Frame(parent, bg="#F0F0F0")
        container.pack(fill=tk.BOTH, expand=True)

        # Header
        header = tk.Frame(container, bg="#F0F0F0")
        header.pack(fill=tk.X)
        self.lbl_alarm_count = tk.Label(header, text="Total Alarms: 0",
                                        font=("Segoe UI", 8), fg="green", bg="#F0F0F0")
        self.lbl_alarm_count.pack(side=tk.LEFT)
        tk.Button(header, text="Clear", command=self._clear_alarms,
                  font=("Segoe UI", 8), relief="flat").pack(side=tk.RIGHT)

        # Alarm list (white background)
        list_frame = tk.Frame(container, bg="white", relief="solid", bd=1)
        list_frame.pack(fill=tk.BOTH, expand=True, pady=(4, 0))

        self.lst_alarms = tk.Listbox(list_frame, font=("Consolas", 8),
                                     bg="white", bd=0, highlightthickness=0)
        self.lst_alarms.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar = ttk.Scrollbar(list_frame, orient="vertical", command=self.lst_alarms.yview)
        self.lst_alarms.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        # Footer
        footer = tk.Frame(container, bg="#F0F0F0")
        footer.pack(fill=tk.X, pady=(4, 0))
        self.chk_auto_scroll = tk.BooleanVar(value=False)  # Default off to reduce lag
        tk.Checkbutton(footer, text="Auto scroll", variable=self.chk_auto_scroll,
                       font=("Segoe UI", 8), bg="#F0F0F0").pack(side=tk.LEFT)

    def _build_traffic_log(self, parent):
        """Traffic Log panel - WHITE background, colored text"""
        container = tk.Frame(parent, bg="#F0F0F0")
        container.pack(fill=tk.BOTH, expand=True)

        # Header
        header = tk.Frame(container, bg="#F0F0F0")
        header.pack(fill=tk.X, pady=(0, 4))

        # Filters (colored)
        filters = tk.Frame(header, bg="#F0F0F0")
        filters.pack(side=tk.LEFT)
        self.chk_tx = tk.BooleanVar(value=True)
        self.chk_rx = tk.BooleanVar(value=True)
        self.chk_sys = tk.BooleanVar(value=True)
        self.chk_warn = tk.BooleanVar(value=True)

        tk.Checkbutton(filters, text="TX", variable=self.chk_tx, fg="#0066CC",
                       font=("Segoe UI", 8, "bold"), command=self._apply_traffic_filter,
                       bg="#F0F0F0", activebackground="#F0F0F0").pack(side=tk.LEFT, padx=(0, 8))
        tk.Checkbutton(filters, text="RX", variable=self.chk_rx, fg="#008800",
                       font=("Segoe UI", 8, "bold"), command=self._apply_traffic_filter,
                       bg="#F0F0F0", activebackground="#F0F0F0").pack(side=tk.LEFT, padx=(0, 8))
        tk.Checkbutton(filters, text="SYS", variable=self.chk_sys, fg="#666666",
                       font=("Segoe UI", 8, "bold"), command=self._apply_traffic_filter,
                       bg="#F0F0F0", activebackground="#F0F0F0").pack(side=tk.LEFT, padx=(0, 8))
        tk.Checkbutton(filters, text="WARN", variable=self.chk_warn, fg="#FF6600",
                       font=("Segoe UI", 8, "bold"), command=self._apply_traffic_filter,
                       bg="#F0F0F0", activebackground="#F0F0F0").pack(side=tk.LEFT)

        # Buttons
        btns = tk.Frame(header, bg="#F0F0F0")
        btns.pack(side=tk.RIGHT)
        tk.Button(btns, text="Save", command=self._save_traffic_log,
                   font=("Segoe UI", 8), relief="flat").pack(side=tk.RIGHT, padx=(4, 0))
        tk.Button(btns, text="Clear", command=self._clear_traffic_log,
                   font=("Segoe UI", 8), relief="flat").pack(side=tk.RIGHT)

        # Traffic grid (white background)
        grid_frame = tk.Frame(container, bg="white", relief="solid", bd=1)
        grid_frame.pack(fill=tk.BOTH, expand=True)

        columns = ("time", "type", "id", "dlc", "data", "info")
        self.tree_traffic = ttk.Treeview(grid_frame, columns=columns, show="headings", height=8)
        widths = {"time": 80, "type": 45, "id": 70, "dlc": 35, "data": 200, "info": 300}
        centered_cols = {"time", "type", "id", "dlc"}
        left_cols = {"data", "info"}

        style = ttk.Style()
        style.configure("TrafficLog.Treeview", font=("Consolas", 9), rowheight=22)
        style.configure("TrafficLog.Treeview.Heading", font=("Segoe UI", 8, "bold"))
        self.tree_traffic.configure(style="TrafficLog.Treeview")

        for col in columns:
            self.tree_traffic.heading(col, text=col.upper(), anchor=tk.CENTER if col in centered_cols else tk.W)
            w = widths.get(col, 100)
            self.tree_traffic.column(
                col,
                width=w,
                minwidth=w,
                stretch=col in left_cols,
                anchor=tk.CENTER if col in centered_cols else tk.W,
            )

        self.traffic_scrollbar = ttk.Scrollbar(grid_frame, orient="vertical", command=self._on_scrollbar_scroll)
        self.tree_traffic.configure(yscrollcommand=self._on_tree_yscroll)
        self.tree_traffic.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self.traffic_scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

        # Tag colors
        self.tree_traffic.tag_configure("TX", foreground="#0066CC")
        self.tree_traffic.tag_configure("RX", foreground="#008800")
        self.tree_traffic.tag_configure("SYS", foreground="#666666")
        self.tree_traffic.tag_configure("ERROR", foreground="#CC0000")
        self.tree_traffic.tag_configure("WARN", foreground="#FF6600")

    # =========================================================================
    # STATUS BAR
    # =========================================================================

    def _build_status_bar(self, parent):
        """Status bar - 28px height"""
        bar = tk.Frame(parent, height=28, bg="#E0E0E0")
        bar.pack(fill=tk.X, pady=(8, 0))
        bar.pack_propagate(False)

        # Separator
        sep = tk.Frame(bar, width=2, bg="#BDBDBD")
        sep.pack(side=tk.LEFT, fill=tk.Y, padx=(0, 8))

        self.lbl_modules = tk.Label(bar, text="Modules: 0", font=("Segoe UI", 8), bg="#E0E0E0")
        self.lbl_modules.pack(side=tk.LEFT)

        sep2 = tk.Frame(bar, width=2, bg="#BDBDBD")
        sep2.pack(side=tk.LEFT, fill=tk.Y, padx=8)

        self.lbl_ok = tk.Label(bar, text="OK", font=("Segoe UI", 9, "bold"),
                               fg="green", bg="#E0E0E0")
        self.lbl_ok.pack(side=tk.LEFT)

        sep_comm = tk.Frame(bar, width=2, bg="#BDBDBD")
        sep_comm.pack(side=tk.LEFT, fill=tk.Y, padx=8)

        self.lbl_comm_summary = tk.Label(
            bar,
            text="TX/RX: 0/0  Timeout: 0  Recovery: 0",
            font=("Segoe UI", 8),
            bg="#E0E0E0",
        )
        self.lbl_comm_summary.pack(side=tk.LEFT)

        # Spacer
        tk.Frame(bar, bg="#E0E0E0").pack(side=tk.LEFT, fill=tk.X, expand=True)

        self.lbl_error_count = tk.Label(bar, text="Errors: 0", font=("Segoe UI", 8), bg="#E0E0E0")
        self.lbl_error_count.pack(side=tk.RIGHT, padx=(0, 8))

        sep3 = tk.Frame(bar, width=2, bg="#BDBDBD")
        sep3.pack(side=tk.RIGHT, fill=tk.Y)

        self.lbl_traffic_count = tk.Label(bar, text="Log Lines: 0", font=("Segoe UI", 8), bg="#E0E0E0")
        self.lbl_traffic_count.pack(side=tk.RIGHT, padx=8)

    # =========================================================================
    # LOGIC METHODS
    # =========================================================================

    def _refresh_ports(self):
        """Refresh available COM ports"""
        ports = self.serial.list_ports()
        self.cmb_port["values"] = ports
        if ports:
            self.cmb_port.current(0)

    def _toggle_connect(self):
        """Toggle connection"""
        if self.serial.is_connected():
            self.serial.send(DebugCmd.EXIT)
            self.serial.disconnect()
            self.btn_connect.configure(text="Connect")
            self.lbl_status.configure(text="Disconnected", fg="red")
            self._add_traffic("SYS", "---", 0, b"", "Disconnected")
            self.bms_data = None
            self.system_info = None
            self.charge_config = None
            self._last_module_rx = 0.0
            self._last_system_rx = 0.0
            self._last_bms_rx = 0.0
            self._update_bms_detail()
        else:
            port = self.cmb_port.get()
            if not port:
                messagebox.showwarning("Warning", "Select a COM port")
                return
            if self.serial.connect(port):
                self.btn_connect.configure(text="Disconnect")
                self.lbl_status.configure(text=f"Connected ({port})", fg="green")
                self._add_traffic("SYS", "---", 0, b"", f"Connected to {port}")
                # ENTER arms the MCU's 1s auto-push of ALL_MODULES/SYSTEM_INFO/
                # BMS_DATA (DebugProtocol_SendStream) — the app no longer polls
                # for these on a timer, it just reacts to what the MCU pushes.
                self.serial.send(DebugCmd.ENTER)
                # Deliberately NOT auto-sending SET_DRIVER/SET_MODULE_ADDR here
                # anymore. SET_DRIVER's handler on the MCU wipes and
                # re-registers the module list as a side effect
                # (ChargeCycleConfig_Set() -> CHG_LIB_Init(), see B-21/B-23 in
                # AUDIT_Findings.md) -- sending it unconditionally on every
                # Connect meant simply opening this app against a charger that
                # was already mid-cycle could silently drop its module
                # registration and, ~10s later, fault it out on a module-count
                # mismatch: connecting the debug tool could stop a live charge
                # session. Confirmed with the user 2026-08-29 (symptom: app
                # always shows "Idle" right after Connect even when the
                # charger is actually running, only recoverable by pressing
                # STOP then START). Now Connect only reads/observes --
                # already-registered modules surface on their own via the
                # ALL_MODULES stream (_update_module_from_data()'s
                # auto-discovery), and SET_DRIVER/SET_MODULE_ADDR are only
                # ever sent when the user explicitly changes the driver
                # dropdown or clicks Add.
                self.root.after(300, self._request_charge_config)

    def _sync_driver(self):
        """Sync selected driver and address to MCU after connect."""
        if not self.serial or not self.serial.is_connected():
            return
        self._on_driver_change()
        self.root.after(50, self._sync_module_addr)

    def _read_addr_entry(self) -> int:
        addr_str = self.ent_addr.get().strip()
        if not addr_str:
            raise ValueError("empty address")

        try:
            addr = int(addr_str, 0)
        except ValueError:
            if all(ch in "0123456789ABCDEFabcdef" for ch in addr_str):
                addr = int(addr_str, 16)
            else:
                raise

        if not (0 <= addr <= 0xFF):
            raise ValueError("address out of range")
        return addr

    def _sync_module_addr(self):
        """Send the current address field as an MCU module."""
        if not self.serial or not self.serial.is_connected():
            return
        try:
            addr = self._read_addr_entry()
        except ValueError:
            self._add_traffic("ERROR", "---", 0, b"", "Invalid module address", "ERROR")
            return
        self.serial.send(StdCmd.SET_MODULE_ADDR, bytes([addr & 0xFF, 0]))
        self._add_traffic("TX", "---", 2, bytes([addr & 0xFF, 0]),
                          f"SET_MODULE_ADDR: 0x{addr & 0xFF:02X}")

    def _request_bms_snapshot(self):
        """Request one-shot BMS snapshot on demand (MCU also auto-pushes this every 1s)."""
        if not self.serial or not self.serial.is_connected():
            messagebox.showwarning("Warning", "Connect to device first")
            return
        self.serial.send(DebugCmd.READ_BMS)
        self._add_traffic("TX", "---", 0, b"", "READ_BMS requested")

    def _request_charge_config(self):
        """Request charge-cycle config snapshot from MCU."""
        if not self.serial or not self.serial.is_connected():
            messagebox.showwarning("Warning", "Connect to device first")
            return
        print(f"[DEBUG] Sending GET_CHARGE_CFG (0x19), connected={self.serial.is_connected()}")
        ok = self.serial.send(DebugCmd.GET_CHARGE_CFG)
        print(f"[DEBUG] GET_CHARGE_CFG send result: {ok}")
        self._add_traffic("TX", "---", 0, b"", "GET_CHARGE_CFG")
        # Show inline status on Charge Config tab if it's built
        if self._charge_config_built and hasattr(self, '_cfg_status_var'):
            self._cfg_status_var.set("⏳ Waiting for MCU response...")
            # Timeout: if no response in 3 seconds, show error in status label
            if hasattr(self, '_cfg_read_timeout_id') and self._cfg_read_timeout_id:
                self.root.after_cancel(self._cfg_read_timeout_id)
            self._cfg_read_timeout_id = self.root.after(3000, self._on_cfg_read_timeout)

    def _on_cfg_read_timeout(self):
        """Called if MCU does not respond to GET_CHARGE_CFG within 3 seconds."""
        self._cfg_read_timeout_id = None
        if self._charge_config_built and hasattr(self, '_cfg_status_var'):
            self._cfg_status_var.set("❌ No response from MCU — check connection")
        print("[DEBUG] GET_CHARGE_CFG timeout: no response from MCU")

    def _load_charge_config_defaults(self):
        defaults = ChargeCycleConfig(
            version=3,
            battery_capacity_ah=0.0,
            imin_c=0.0,
            imax_c=0.0,
            ipre_c=0.0,
            ilow_c=0.0,
            vmin_v=0.0,
            vmax_v=0.0,
            vpre_v=0.0,
            vlow_v=0.0,
            temp_limit_c=0.0,
            cell_volt_enabled=False,
            cell_volt_delta_v=0.0,
            cell_volt_1_v=0.0,
            cell_volt_2_v=0.0,
            cell_volt_3_v=0.0,
            cell_volt_4_v=0.0,
            cell_volt_5_v=0.0,
            cell_curr_1_c=0.0,
            cell_curr_2_c=0.0,
            cell_curr_3_c=0.0,
            cell_curr_4_c=0.0,
            temp_enabled=False,
            temp_delta_c=0.0,
            temp_1_c=0.0,
            temp_2_c=0.0,
            temp_3_c=0.0,
            temp_4_c=0.0,
            temp_5_c=0.0,
            temp_curr_1_c=0.0,
            temp_curr_2_c=0.0,
            temp_curr_3_c=0.0,
            temp_curr_4_c=0.0,
            soc_enabled=False,
            soc_delta_pct=0.0,
            soc_1_pct=0.0,
            soc_2_pct=0.0,
            soc_3_pct=0.0,
            soc_4_pct=0.0,
            soc_5_pct=0.0,
            soc_curr_1_c=0.0,
            soc_curr_2_c=0.0,
            soc_curr_3_c=0.0,
            soc_curr_4_c=0.0,
            protect_jack_charge_enabled=False,
            protect_jack_charge_delta_v=0.0,
            protect_jack_charge_delay_s=0,
            protect_jack_temp_enabled=False,
            protect_jack_temp_delay_s=0,
            protect_jack_temp_threshold_c=0.0,
            protect_jack_temp_delta_c=0.0,
            protect_jack_temp_power_limit_pct=80.0,
            charge_source_mode=0,
            can_battery_id=1,
            source_module_count=1,
            module_type=1,
            module_u_min_v=30.0,
            module_u_max_v=99.0,
            module_i_min_a=5.0,
            module_i_max_a=100.0,
        )
        self._load_charge_config_to_ui(defaults)

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
        self._apply_charge_source_mode_ui()

    def _read_cfg_float(self, key: str) -> float:
        return float(self.cfg_vars[key].get().strip())

    def _read_cfg_int(self, key: str) -> int:
        return int(self.cfg_vars[key].get().strip())

    def _collect_charge_config_from_ui(self) -> ChargeCycleConfig:
        module_name = self.cfg_module_type.get()
        module_type = next((idx for idx, name in MODULE_TYPE_NAMES.items() if name == module_name), 0)
        charge_source_name = self.cfg_charge_source_mode.get()
        charge_source_mode = next((idx for idx, name in CHARGE_SOURCE_MODE_NAMES.items() if name == charge_source_name), 0)
        return ChargeCycleConfig(
            version=3,
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
            charge_source_mode=charge_source_mode,
            can_battery_id=self._read_cfg_int("can_battery_id"),
            source_module_count=self._read_cfg_int("source_module_count"),
            module_type=module_type,
            module_u_min_v=self._read_cfg_float("module_u_min_v"),
            module_u_max_v=self._read_cfg_float("module_u_max_v"),
            module_i_min_a=self._read_cfg_float("module_i_min_a"),
            module_i_max_a=self._read_cfg_float("module_i_max_a"),
        )

    def _export_charge_config(self):
        """Export charge config to file"""
        try:
            cfg = self._collect_charge_config_from_ui()
        except ValueError:
            messagebox.showerror("Error", "Charge config contains invalid numeric values")
            return

        file_path = filedialog.asksaveasfilename(
            title="Export Charge Config",
            defaultextension=".json",
            filetypes=[("JSON files", "*.json"), ("All files", "*.*")],
            initialfile="charge_config.json"
        )
        if not file_path:
            return

        try:
            import json
            data = {
                "version": cfg.version,
                "battery_capacity_ah": cfg.battery_capacity_ah,
                "imin_c": cfg.imin_c,
                "imax_c": cfg.imax_c,
                "ipre_c": cfg.ipre_c,
                "ilow_c": cfg.ilow_c,
                "vmin_v": cfg.vmin_v,
                "vmax_v": cfg.vmax_v,
                "vpre_v": cfg.vpre_v,
                "vlow_v": cfg.vlow_v,
                "temp_limit_c": cfg.temp_limit_c,
                "cell_volt_enabled": cfg.cell_volt_enabled,
                "cell_volt_delta_v": cfg.cell_volt_delta_v,
                "cell_volt_1_v": cfg.cell_volt_1_v,
                "cell_volt_2_v": cfg.cell_volt_2_v,
                "cell_volt_3_v": cfg.cell_volt_3_v,
                "cell_volt_4_v": cfg.cell_volt_4_v,
                "cell_volt_5_v": cfg.cell_volt_5_v,
                "cell_curr_1_c": cfg.cell_curr_1_c,
                "cell_curr_2_c": cfg.cell_curr_2_c,
                "cell_curr_3_c": cfg.cell_curr_3_c,
                "cell_curr_4_c": cfg.cell_curr_4_c,
                "temp_enabled": cfg.temp_enabled,
                "temp_delta_c": cfg.temp_delta_c,
                "temp_1_c": cfg.temp_1_c,
                "temp_2_c": cfg.temp_2_c,
                "temp_3_c": cfg.temp_3_c,
                "temp_4_c": cfg.temp_4_c,
                "temp_5_c": cfg.temp_5_c,
                "temp_curr_1_c": cfg.temp_curr_1_c,
                "temp_curr_2_c": cfg.temp_curr_2_c,
                "temp_curr_3_c": cfg.temp_curr_3_c,
                "temp_curr_4_c": cfg.temp_curr_4_c,
                "soc_enabled": cfg.soc_enabled,
                "soc_delta_pct": cfg.soc_delta_pct,
                "soc_1_pct": cfg.soc_1_pct,
                "soc_2_pct": cfg.soc_2_pct,
                "soc_3_pct": cfg.soc_3_pct,
                "soc_4_pct": cfg.soc_4_pct,
                "soc_5_pct": cfg.soc_5_pct,
                "soc_curr_1_c": cfg.soc_curr_1_c,
                "soc_curr_2_c": cfg.soc_curr_2_c,
                "soc_curr_3_c": cfg.soc_curr_3_c,
                "soc_curr_4_c": cfg.soc_curr_4_c,
                "protect_jack_charge_enabled": cfg.protect_jack_charge_enabled,
                "protect_jack_charge_delta_v": cfg.protect_jack_charge_delta_v,
                "protect_jack_charge_delay_s": cfg.protect_jack_charge_delay_s,
                "protect_jack_temp_enabled": cfg.protect_jack_temp_enabled,
                "protect_jack_temp_delay_s": cfg.protect_jack_temp_delay_s,
                "protect_jack_temp_threshold_c": cfg.protect_jack_temp_threshold_c,
                "protect_jack_temp_delta_c": cfg.protect_jack_temp_delta_c,
                "protect_jack_temp_power_limit_pct": cfg.protect_jack_temp_power_limit_pct,
                "charge_source_mode": cfg.charge_source_mode,
                "can_battery_id": cfg.can_battery_id,
                "source_module_count": cfg.source_module_count,
                "module_type": cfg.module_type,
                "module_u_min_v": cfg.module_u_min_v,
                "module_u_max_v": cfg.module_u_max_v,
                "module_i_min_a": cfg.module_i_min_a,
                "module_i_max_a": cfg.module_i_max_a,
            }
            with open(file_path, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=2)
            messagebox.showinfo("Success", f"Config exported to:\n{file_path}")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to export config:\n{str(e)}")

    def _import_charge_config(self):
        """Import charge config from file"""
        file_path = filedialog.askopenfilename(
            title="Import Charge Config",
            filetypes=[("JSON files", "*.json"), ("All files", "*.*")]
        )
        if not file_path:
            return

        try:
            import json
            with open(file_path, 'r', encoding='utf-8') as f:
                data = json.load(f)

            cfg = ChargeCycleConfig(
                version=data.get("version", 3),
                battery_capacity_ah=data.get("battery_capacity_ah", 0.0),
                imin_c=data.get("imin_c", 0.0),
                imax_c=data.get("imax_c", 0.0),
                ipre_c=data.get("ipre_c", 0.0),
                ilow_c=data.get("ilow_c", 0.0),
                vmin_v=data.get("vmin_v", 0.0),
                vmax_v=data.get("vmax_v", 0.0),
                vpre_v=data.get("vpre_v", 0.0),
                vlow_v=data.get("vlow_v", 0.0),
                temp_limit_c=data.get("temp_limit_c", 0.0),
                cell_volt_enabled=data.get("cell_volt_enabled", False),
                cell_volt_delta_v=data.get("cell_volt_delta_v", 0.0),
                cell_volt_1_v=data.get("cell_volt_1_v", 0.0),
                cell_volt_2_v=data.get("cell_volt_2_v", 0.0),
                cell_volt_3_v=data.get("cell_volt_3_v", 0.0),
                cell_volt_4_v=data.get("cell_volt_4_v", 0.0),
                cell_volt_5_v=data.get("cell_volt_5_v", 0.0),
                cell_curr_1_c=data.get("cell_curr_1_c", 0.0),
                cell_curr_2_c=data.get("cell_curr_2_c", 0.0),
                cell_curr_3_c=data.get("cell_curr_3_c", 0.0),
                cell_curr_4_c=data.get("cell_curr_4_c", 0.0),
                temp_enabled=data.get("temp_enabled", False),
                temp_delta_c=data.get("temp_delta_c", 0.0),
                temp_1_c=data.get("temp_1_c", 0.0),
                temp_2_c=data.get("temp_2_c", 0.0),
                temp_3_c=data.get("temp_3_c", 0.0),
                temp_4_c=data.get("temp_4_c", 0.0),
                temp_5_c=data.get("temp_5_c", 0.0),
                temp_curr_1_c=data.get("temp_curr_1_c", 0.0),
                temp_curr_2_c=data.get("temp_curr_2_c", 0.0),
                temp_curr_3_c=data.get("temp_curr_3_c", 0.0),
                temp_curr_4_c=data.get("temp_curr_4_c", 0.0),
                soc_enabled=data.get("soc_enabled", False),
                soc_delta_pct=data.get("soc_delta_pct", 0.0),
                soc_1_pct=data.get("soc_1_pct", 0.0),
                soc_2_pct=data.get("soc_2_pct", 0.0),
                soc_3_pct=data.get("soc_3_pct", 0.0),
                soc_4_pct=data.get("soc_4_pct", 0.0),
                soc_5_pct=data.get("soc_5_pct", 0.0),
                soc_curr_1_c=data.get("soc_curr_1_c", 0.0),
                soc_curr_2_c=data.get("soc_curr_2_c", 0.0),
                soc_curr_3_c=data.get("soc_curr_3_c", 0.0),
                soc_curr_4_c=data.get("soc_curr_4_c", 0.0),
                protect_jack_charge_enabled=data.get("protect_jack_charge_enabled", False),
                protect_jack_charge_delta_v=data.get("protect_jack_charge_delta_v", 0.0),
                protect_jack_charge_delay_s=data.get("protect_jack_charge_delay_s", 0),
                protect_jack_temp_enabled=data.get("protect_jack_temp_enabled", False),
                protect_jack_temp_delay_s=data.get("protect_jack_temp_delay_s", 0),
                protect_jack_temp_threshold_c=data.get("protect_jack_temp_threshold_c", 0.0),
                protect_jack_temp_delta_c=data.get("protect_jack_temp_delta_c", 0.0),
                protect_jack_temp_power_limit_pct=data.get("protect_jack_temp_power_limit_pct", 80.0),
                charge_source_mode=data.get("charge_source_mode", 0),
                can_battery_id=data.get("can_battery_id", 1),
                source_module_count=data.get("source_module_count", 1),
                module_type=data.get("module_type", 1),
                module_u_min_v=data.get("module_u_min_v", 30.0),
                module_u_max_v=data.get("module_u_max_v", 99.0),
                module_i_min_a=data.get("module_i_min_a", 5.0),
                module_i_max_a=data.get("module_i_max_a", 100.0),
            )
            self._load_charge_config_to_ui(cfg)
            messagebox.showinfo("Success", f"Config imported from:\n{file_path}")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to import config:\n{str(e)}")

    def _send_charge_config(self):
        if not self.serial or not self.serial.is_connected():
            messagebox.showwarning("Warning", "Connect to device first")
            return
        try:
            cfg = self._collect_charge_config_from_ui()
        except ValueError:
            messagebox.showerror("Error", "Charge config contains invalid numeric values")
            return

        payload = cfg.to_bytes()
        self.serial.send(DebugCmd.SET_CHARGE_CFG, payload)
        self._add_traffic("TX", "---", len(payload), payload, "SET_CHARGE_CFG")

    def _on_driver_change(self, event=None):
        """Handle driver selection"""
        drivers = {"Maxwell": 1, "Lianming": 2, "TonHe": 3}
        name = self.cmb_driver.get()
        self.driver_id = drivers.get(name, 1)
        if self.serial and self.serial.is_connected():
            self.serial.send(StdCmd.SET_DRIVER, bytes([self.driver_id]))
            self._add_traffic("TX", "---", 1, bytes([self.driver_id]),
                              f"SET_DRIVER: {name} ({self.driver_id})")
            if event is not None:
                self.root.after(50, self._sync_module_addr)

    def _add_module(self):
        """Add module"""
        try:
            addr = self._read_addr_entry()
            # An explicit Add is the user's way of saying "yes, track this
            # one again" -- undo any earlier Remove/Clear for this exact
            # key so the ALL_MODULES auto-discovery path (see
            # _update_module_from_data()) resumes updating it normally.
            self._user_removed_keys.discard((addr, self.driver_id))

            # If this addr+driver is already tracked locally (e.g. a repeat
            # click, or the connect-time auto-sync -- see _sync_module_addr()
            # -- already told the MCU about it before this button was ever
            # pressed), treat it as idempotent: re-select/re-sync the
            # existing row instead of hard-blocking with a warning dialog.
            # Blocking here used to leave the operator stuck with an empty,
            # unselectable module table and no way to reach START, even
            # though the module was already correctly registered on the MCU.
            for idx_existing, mod in self.modules.items():
                if mod.addr == addr and mod.driver == self.driver_id:
                    mod.user_added = True
                    if self.serial and self.serial.is_connected():
                        self.serial.send(StdCmd.SET_MODULE_ADDR, bytes([addr & 0xFF, 0]))
                        self._add_traffic("TX", "---", 2, bytes([addr & 0xFF, 0]),
                                          f"SET_MODULE_ADDR (re-sync): 0x{addr & 0xFF:02X}")
                    self._update_module_grid()
                    self._set_selected_module(idx_existing)
                    self._add_traffic("SYS", f"{addr:03X}", 0, b"",
                                    f"Module already tracked, re-selected: {DRIVER_NAMES[self.driver_id]} 0x{addr:02X}")
                    return

            idx = 0
            while idx in self.modules:
                idx += 1
            mod = ChargerModule(addr=addr, driver=self.driver_id, module_idx=idx, user_added=True)
            self.modules[idx] = mod
            # Update O(1) lookup index
            self._module_lookup[(addr, self.driver_id)] = idx

            if self.serial and self.serial.is_connected():
                self.serial.send(StdCmd.SET_MODULE_ADDR, bytes([addr & 0xFF, 0]))
                self._add_traffic("TX", "---", 2, bytes([addr & 0xFF, 0]),
                                  f"SET_MODULE_ADDR: 0x{addr & 0xFF:02X}")

            self._update_module_grid()
            self._set_selected_module(idx)
            self._add_traffic("SYS", f"{addr:03X}", 0, b"",
                            f"Module added: {DRIVER_NAMES[self.driver_id]} 0x{addr:02X}")
        except ValueError:
            messagebox.showerror("Error", "Invalid address. Use 0x01 or 1-255.")

    def _remove_module(self):
        """Remove selected module.

        Note: this only forgets the module on the app side -- the wire
        protocol has no "un-register module" command, so the MCU keeps
        reporting it via the ALL_MODULES stream (~1s) exactly as before.
        Without _user_removed_keys, _update_module_from_data() would just
        auto-recreate the same entry on the very next stream frame, making
        Remove look broken (module reappears on its own). Recording the key
        here tells that auto-discovery path to leave it alone until the
        user explicitly re-Adds it (see _add_module()).
        """
        selection = self.tree_modules.selection()
        if not selection:
            return
        item = selection[0]
        tags = self.tree_modules.item(item, "tags")
        if tags:
            idx = int(tags[0])
            if idx in self.modules:
                mod = self.modules[idx]
                # Remove from lookup dict
                key = (mod.addr, mod.driver)
                self._module_lookup.pop(key, None)
                del self.modules[idx]
                self._user_removed_keys.add(key)
                if self.selected_module_idx == idx:
                    self.selected_module_idx = None
                self._update_module_grid()
                self._update_detail()
                self._check_alarms()

    def _clear_modules(self):
        """Clear all modules. Same MCU-can't-forget-it caveat as
        _remove_module() -- mark every key as user-removed so the
        ALL_MODULES stream doesn't silently repopulate the table."""
        self._user_removed_keys.update(self._module_lookup.keys())
        self.modules.clear()
        self._module_lookup.clear()  # Clear lookup index
        self.selected_module_idx = None
        self._update_module_grid()
        self._update_detail()
        self._check_alarms()

    def _on_module_select(self, event):
        """Handle module selection"""
        if self._syncing_module_selection:
            return
        tree = event.widget
        selection = tree.selection()
        if not selection:
            return
        item = selection[0]
        tags = tree.item(item, "tags")
        if tags:
            idx = int(tags[0])
            if idx == self.selected_module_idx:
                return
            self._set_selected_module(idx)

    def _set_selected_module(self, idx: Optional[int]):
        normalized_idx = idx if idx in self.modules else None
        if normalized_idx == self.selected_module_idx:
            return
        self.selected_module_idx = normalized_idx
        self._sync_module_selection_views()
        self._update_detail()
        # A different module's V/I history is a different chart -- don't mix
        # them. Reset lazily (only touch the canvas if the tab exists).
        self.charge_graph_history.clear()
        self._charge_graph_t0 = None
        self._charge_graph_module_idx = normalized_idx
        if self._charge_graph_built:
            self._redraw_charge_graph()

    def _sync_module_selection_views(self):
        self._syncing_module_selection = True
        try:
            for tree_name in ("tree_modules", "tree_monitor_modules"):
                tree = getattr(self, tree_name, None)
                if tree is None:
                    continue
                tree.selection_remove(*tree.selection())
                if self.selected_module_idx is None:
                    continue
                for item in tree.get_children():
                    tags = tree.item(item, "tags")
                    if tags and int(tags[0]) == self.selected_module_idx:
                        tree.selection_set(item)
                        tree.focus(item)
                        tree.see(item)
                        break
        finally:
            self._syncing_module_selection = False

    def _update_module_grid(self):
        """Update module list grid - only show user-added modules"""
        trees = [tree for tree in (getattr(self, "tree_modules", None), getattr(self, "tree_monitor_modules", None)) if tree is not None]
        for tree in trees:
            existing = {int(tree.item(item, "tags")[0]): item for item in tree.get_children() if tree.item(item, "tags")}
            seen_idx = set()
            
            for idx, mod in sorted(self.modules.items()):
                if not mod.user_added:
                    continue
                seen_idx.add(idx)
                
                # Determine state tag for color
                if mod.state == 5:  # FAULT
                    state_tag = "fault"
                    online = "FLT"
                elif mod.online and mod.running:
                    state_tag = "running"
                    online = "OK"
                elif mod.online:
                    state_tag = "online"
                    online = "ON"
                else:
                    state_tag = "offline"
                    online = "--"
                    
                values = (f"0x{mod.addr:02X}", mod.get_driver_name()[:3], online)
                
                if idx in existing:
                    tree.item(existing[idx], values=values, tags=(str(idx), state_tag))
                else:
                    tree.insert("", "end", values=values, tags=(str(idx), state_tag))
                    
            for idx, item in existing.items():
                if idx not in seen_idx:
                    tree.delete(item)

        # Count only user-added modules
        user_count = sum(1 for m in self.modules.values() if m.user_added)
        self.lbl_modules.configure(text=f"Modules: {user_count}")
        self._sync_module_selection_views()

    def _set_voltage(self):
        """Set voltage"""
        if self.selected_module_idx is None:
            messagebox.showwarning("Warning", "Select a module first")
            return
        try:
            v = float(self.ent_voltage.get())
            self.serial.send(StdCmd.SET_VOLTAGE, struct.pack("<f", v))
            self._add_traffic("TX", "---", 8, struct.pack("<f", v), f"Set Voltage: {v}V")
            self.root.after(100, self._force_quick_poll)
        except ValueError:
            messagebox.showerror("Error", "Invalid voltage")

    def _set_current(self):
        """Set current"""
        if self.selected_module_idx is None:
            messagebox.showwarning("Warning", "Select a module first")
            return
        try:
            i = float(self.ent_current.get())
            self.serial.send(StdCmd.SET_CURRENT, struct.pack("<f", i))
            self._add_traffic("TX", "---", 8, struct.pack("<f", i), f"Set Current: {i}A")
            self.root.after(100, self._force_quick_poll)
        except ValueError:
            messagebox.showerror("Error", "Invalid current")

    def _start(self):
        """Start charging"""
        if self.selected_module_idx is None:
            messagebox.showwarning("Warning", "Select a module first")
            return
        mode = 1 if self.var_manual_mode.get() else 0
        self.serial.send(StdCmd.START, bytes([mode]))
        self._add_traffic("TX", "---", 1, bytes([mode]), f"START command (Mode: {'Manual' if mode else 'Auto'})")
        self.root.after(100, self._force_quick_poll)

    def _stop(self):
        """Stop charging"""
        if self.selected_module_idx is None:
            messagebox.showwarning("Warning", "Select a module first")
            return
        self.serial.send(StdCmd.STOP)
        self._add_traffic("TX", "---", 0, b"", "STOP command")
        self.root.after(100, self._force_quick_poll)

    def _estop(self):
        """Emergency stop"""
        self.serial.send(StdCmd.EMERGENCY_STOP)
        self._add_traffic("TX", "---", 0, b"", "EMERGENCY STOP", "ERROR")
        self.root.after(100, self._force_quick_poll)

    def _force_quick_poll(self, count=0):
        """One-shot request for fresh module/system/BMS data right after a user
        command, so the UI doesn't have to wait for the next 1s auto-push."""
        if self.serial and self.serial.is_connected():
            self.serial.send(DebugCmd.READ_ALL)
            self.serial.send(DebugCmd.GET_SYSTEM)
            self.serial.send(DebugCmd.READ_BMS)

    def _set_var(self, var: tk.StringVar, new_value: str):
        """Only call var.set() if value actually changed — avoids unnecessary widget redraws."""
        if var.get() != new_value:
            var.set(new_value)

    def _update_detail(self):
        """Update module detail panel"""
        mod = self.modules.get(self.selected_module_idx)

        if mod is None:
            for var in self.detail_vars.values():
                self._set_var(var, "---")
            for key in ["TX Count", "RX Count", "Error Count", "Timeout Count", "Recovery Count"]:
                self._set_var(self._ensure_stats_var(key), "0")
            if hasattr(self, "lbl_comm_summary"):
                self.lbl_comm_summary.configure(text="TX/RX: 0/0  Timeout: 0  Recovery: 0")
            return

        # Basic
        self._set_var(self.detail_vars["Driver"], mod.get_driver_name())
        self._set_var(self.detail_vars["State"], mod.get_state_name())
        self._set_var(self.detail_vars["Online"], "Yes" if mod.online else "No")
        self._set_var(self.detail_vars["Running"], "Yes" if mod.running else "No")
        self._set_var(self.detail_vars["Voltage"], f"{mod.voltage:.2f}" if mod.voltage else "---")
        self._set_var(self.detail_vars["Current"], f"{mod.current:.2f}" if mod.current else "---")
        self._set_var(self.detail_vars["Curr Limit"], f"{mod.current_limit:.2f}" if mod.current_limit else "---")

        # Temperatures
        self._set_var(self.detail_vars["Temp DCDC"], f"{mod.temp_dcdc:.1f}" if mod.temp_dcdc else "---")
        self._set_var(self.detail_vars["Temp Ambient"], f"{mod.temp_ambient:.1f}" if mod.temp_ambient else "---")
        self._set_var(self.detail_vars["Temp PFC"], f"{mod.temp_pfc:.1f}" if mod.temp_pfc else "---")

        # AC Input
        self._set_var(self.detail_vars["AC Phase A"], f"{mod.ac_phase_a:.1f}" if mod.ac_phase_a else "---")
        self._set_var(self.detail_vars["AC Phase B"], f"{mod.ac_phase_b:.1f}" if mod.ac_phase_b else "---")
        self._set_var(self.detail_vars["AC Phase C"], f"{mod.ac_phase_c:.1f}" if mod.ac_phase_c else "---")

        # Power
        self._set_var(self.detail_vars["Input Power"], f"{mod.input_power:.0f}" if mod.input_power else "---")
        self._set_var(self.detail_vars["Rated Power"], f"{mod.rated_power:.0f}" if mod.rated_power else "---")
        self._set_var(self.detail_vars["Rated Current"], f"{mod.rated_current:.1f}" if mod.rated_current else "---")

        # PFC Bus
        self._set_var(self.detail_vars["PFC Bus +"], f"{mod.pfc_bus_plus:.1f}" if mod.pfc_bus_plus else "---")
        self._set_var(self.detail_vars["PFC Bus -"], f"{mod.pfc_bus_minus:.1f}" if mod.pfc_bus_minus else "---")
        self._set_var(self.detail_vars["PFC Fault"], f"0x{mod.pfc_fault:02X}" if mod.pfc_fault else "---")

        # Alarms
        self._set_var(self.detail_vars["Alarm Summary"], mod.get_alarm_summary())

        # Stats
        self._set_var(self._ensure_stats_var("TX Count"), str(mod.tx_count))
        self._set_var(self._ensure_stats_var("RX Count"), str(mod.rx_count))
        self._set_var(self._ensure_stats_var("Error Count"), str(mod.error_count))
        self._set_var(self._ensure_stats_var("Timeout Count"), str(mod.timeout_count))
        self._set_var(self._ensure_stats_var("Recovery Count"), str(mod.recovery_count))
        if hasattr(self, "lbl_comm_summary"):
            self.lbl_comm_summary.configure(
                text=f"TX/RX: {mod.tx_count}/{mod.rx_count}  Timeout: {mod.timeout_count}  Recovery: {mod.recovery_count}"
            )

    def _update_alarm_list(self):
        """Update alarm list"""
        if not hasattr(self, "lst_alarms"):
            if not self.alarms:
                self.lbl_ok.configure(text="OK", fg="green")
            else:
                has_fault = any(a.level == "FAULT" for a in self.alarms)
                self.lbl_ok.configure(text="FAULT" if has_fault else "WARN",
                                      fg="#CC0000" if has_fault else "#FF6600")
            return

        self.lst_alarms.delete(0, tk.END)

        if not self.alarms:
            self.lst_alarms.insert(0, "No active alarms")
            self.lst_alarms.itemconfig(0, fg="#4CAF50")
            self.lbl_alarm_count.configure(text="Total Alarms: 0", fg="green")
            self.lbl_ok.configure(text="OK", fg="green")
        else:
            for alarm in self.alarms:
                text = f"{alarm.num:<4} {alarm.timestamp:<8} {alarm.module:<8} {alarm.level:<6} {alarm.message}"
                self.lst_alarms.insert(tk.END, text)
                colors = {"FAULT": "#CC0000", "WARN": "#FF6600", "INFO": "#0066CC", "RECOVER": "#008800"}
                self.lst_alarms.itemconfig(tk.END, fg=colors.get(alarm.level, "black"))

            self.lbl_alarm_count.configure(text=f"Total Alarms: {len(self.alarms)}", fg="#CC0000")
            has_fault = any(a.level == "FAULT" for a in self.alarms)
            self.lbl_ok.configure(text="FAULT" if has_fault else "WARN",
                                fg="#CC0000" if has_fault else "#FF6600")

            if self.chk_auto_scroll.get():
                self.lst_alarms.see(tk.END)

    def _clear_alarms(self):
        """Clear alarms"""
        self.alarms.clear()
        self.active_alarm_keys.clear()
        self._update_alarm_list()

    def _add_traffic(self, type_: str, id_hex: str, dlc: int, data: bytes, info: str, tag: str = None):
        """Add traffic log entry"""
        if tag is None:
            tag = type_
        if tag not in self.traffic_filter or not self.traffic_filter[tag]:
            return

        # Cache timestamp - only update every second
        now = time.time()
        if now - self._last_timestamp_update >= 1.0:
            self._last_timestamp = time.strftime("%H:%M:%S")
            self._last_timestamp_update = now

        # Track error count for caching
        if type_ == "ERROR":
            self._error_count += 1

        entry = TrafficEntry(
            time=self._last_timestamp,
            type=type_,
            id_hex=id_hex,
            dlc=dlc,
            data_hex=data.hex().upper() if data else "",
            module="",
            info=info
        )
        self.traffic.append(entry)

        # Buffer UI updates instead of full refresh
        self._pending_traffic_inserts.append(entry)
        self._traffic_dirty = True

    def _on_scrollbar_scroll(self, *args):
        """Called when user interacts with the scrollbar"""
        self._user_scrolling_traffic = True
        self.tree_traffic.yview(*args)
        self._reset_scroll_timer()

    def _on_tree_yscroll(self, first, last):
        """Called when treeview scroll position changes"""
        self.traffic_scrollbar.set(first, last)
        
        # If we reached the bottom, user isn't scrolling away anymore
        if float(last) >= 1.0:
            self._user_scrolling_traffic = False

    def _reset_scroll_timer(self):
        if hasattr(self, '_scroll_timer'):
            self.root.after_cancel(self._scroll_timer)
        self._scroll_timer = self.root.after(1000, self._clear_scroll_flag)

    def _clear_scroll_flag(self):
        self._user_scrolling_traffic = False

    def _apply_traffic_filter(self):
        """Apply traffic filter"""
        self.traffic_filter["TX"] = self.chk_tx.get()
        self.traffic_filter["RX"] = self.chk_rx.get()
        self.traffic_filter["SYS"] = self.chk_sys.get()
        self.traffic_filter["WARN"] = self.chk_warn.get()
        self._refresh_traffic_view()

    def _refresh_traffic_view(self):
        """Refresh traffic grid"""
        self.tree_traffic.delete(*self.tree_traffic.get_children())
        for entry in self.traffic:
            if entry.type in self.traffic_filter and not self.traffic_filter[entry.type]:
                continue
            self.tree_traffic.insert("", "end", values=(
                entry.time, entry.type, entry.id_hex, entry.dlc, entry.data_hex, entry.info
            ), tags=(entry.type,))

        self.lbl_traffic_count.configure(text=f"Log Lines: {len(self.traffic)}")
        self.lbl_error_count.configure(text=f"Errors: {self._error_count}")

    def _update_bms_detail(self):
        """Refresh BMS tab from latest BMS snapshot."""
        if self.bms_data is None:
            for var in self.bms_vars.values():
                self._set_var(var, "---")
            if self._last_bms_alarm_flags is not None:
                self._last_bms_alarm_flags = None
                self.lst_bms_alarms.delete(0, tk.END)
                self.lst_bms_alarms.insert(0, "No BMS data")
                self.lst_bms_alarms.itemconfig(0, fg="#666666")
                self.lbl_bms_alarm_state.configure(text="No BMS alarms", fg="green")
            self._update_charge_process()
            return

        bms = self.bms_data
        self._set_var(self.bms_vars["State"], bms.get_state_name())
        self._set_var(self.bms_vars["Online"], "Yes" if bms.online else "No")
        self._set_var(self.bms_vars["SOC"], str(bms.soc))
        self._set_var(self.bms_vars["SOH"], str(bms.soh))
        self._set_var(self.bms_vars["Batt Voltage"], f"{bms.batt_voltage:.2f}")
        self._set_var(self.bms_vars["Batt Current"], f"{bms.batt_current:.2f}")
        self._set_var(self.bms_vars["Cap Remain"], f"{bms.cap_remain:.1f}")
        self._set_var(self.bms_vars["Rate Capacity"], f"{bms.rate_cap:.1f}")
        self._set_var(self.bms_vars["Req Voltage"], f"{bms.chg_volt_request:.2f}")
        self._set_var(self.bms_vars["Req Current"], f"{bms.chg_curr_request:.2f}")
        self._set_var(self.bms_vars["Last RX Tick"], str(bms.last_rx_tick))
        self._set_var(self.bms_vars["Max Cell Volt"], str(bms.max_cell_volt))
        self._set_var(self.bms_vars["Min Cell Volt"], str(bms.min_cell_volt))
        self._set_var(self.bms_vars["Max Cell Temp"], f"{bms.max_cell_temp:.1f}")
        self._set_var(self.bms_vars["Min Cell Temp"], f"{bms.min_cell_temp:.1f}")
        self._set_var(self.bms_vars["Charge Relay"], "Closed" if bms.charge_relay_closed else "Open")
        self._set_var(self.bms_vars["Discharge Relay"], "Closed" if bms.discharge_relay_closed else "Open")

        alarm_names = bms.get_alarm_names()
        self._set_var(self.bms_vars["Alarm Count"], str(len(alarm_names)))
        self._set_var(self.bms_vars["Cell Delta"],
            str(bms.max_cell_volt - bms.min_cell_volt) if bms.max_cell_volt and bms.min_cell_volt else "---"
        )
        self._set_var(self.bms_vars["Temp Delta"], f"{bms.max_cell_temp - bms.min_cell_temp:.1f}")
        self._set_var(self.bms_vars["BMS Alarms"], "; ".join(alarm_names) if alarm_names else "None")

        # Only rebuild BMS alarm listbox if alarm_flags changed
        if self._last_bms_alarm_flags != bms.alarm_flags:
            self._last_bms_alarm_flags = bms.alarm_flags
            self.lst_bms_alarms.delete(0, tk.END)
            if alarm_names:
                for idx, name in enumerate(alarm_names, start=1):
                    self.lst_bms_alarms.insert(tk.END, f"{idx:02d}. {name}")
                    self.lst_bms_alarms.itemconfig(tk.END, fg="#CC0000")
                self.lbl_bms_alarm_state.configure(text=f"BMS alarms: {len(alarm_names)}", fg="#CC0000")
            else:
                self.lst_bms_alarms.insert(0, "No active BMS alarms")
                self.lst_bms_alarms.itemconfig(0, fg="#4CAF50")
                self.lbl_bms_alarm_state.configure(text="No BMS alarms", fg="green")

        self._update_charge_process()

    def _is_monitor_stale(self, last_rx):
        if not self.serial or not self.serial.is_connected() or not last_rx:
            return True
        return ((time.monotonic() - last_rx) * 1000.0) > self._monitor_stale_ms

    def _update_charge_process(self):
        if not self.process_vars:
            return

        for key, default in (
            ("Controller State", "---"),
            ("Control Mode", "---"),
            ("Process Summary", "---"),
            ("Modules Online", str(sum(1 for m in self.modules.values() if m.online))),
            ("BMS Status", "---"),
            ("BMS Voltage Target", "---"),
            ("BMS Current Limit", "---"),
            ("Battery Max Temp", "---"),
            ("Controller Voltage Target", "---"),
            ("Controller Current Target", "---"),
            ("Charge Status", "---"),
            ("Active Logic", "---"),
            ("Charge Level", "---"),
            ("Current Limit", "---"),
            ("Stop Reason", "---"),
            ("Controller Fault", "---"),
            ("Actual Charge Voltage", "---"),
            ("Actual Charge Current", "---"),
            ("Charger Max Temp", "---"),
        ):
            self._set_var(self.process_vars[key], default)

        info = self.system_info
        bms = self.bms_data

        if bms is not None:
            bms_status = bms.get_state_name()
            if bms.online:
                bms_status = "ONLINE / STALE" if (bms.alarm_flags & (1 << 14)) else "ONLINE"
            else:
                bms_status = "OFFLINE"
            self._set_var(self.process_vars["BMS Status"], bms_status)
            self._set_var(self.process_vars["BMS Voltage Target"], f"{bms.chg_volt_request:.2f}")
            self._set_var(self.process_vars["BMS Current Limit"], f"{bms.chg_curr_request:.2f}")
            if bms:
                self._set_var(self.process_vars["Battery Max Temp"], f"{bms.max_cell_temp:.1f}")

        if info is None:
            if self.serial and self.serial.is_connected() and self._is_monitor_stale(self._last_system_rx):
                self._set_var(self.process_vars["Process Summary"], "SYSTEM_INFO timeout")
            return

        if info is not None:
            self._set_var(self.process_vars["Controller State"], info.get_controller_state_name())
            self._set_var(self.process_vars["Control Mode"], info.get_charge_source_mode_name())
            self._set_var(self.process_vars["Modules Online"], f"{info.modules_online}/{info.modules_total}")
            self._set_var(self.process_vars["Controller Voltage Target"],
                f"{info.controller_target_voltage:.2f}" if info.controller_target_voltage else "---"
            )
            self._set_var(self.process_vars["Controller Current Target"],
                f"{info.controller_target_current_total:.2f}" if info.controller_target_current_total else "---"
            )
            self._set_var(self.process_vars["Charge Status"], info.get_charge_status_name())
            self._set_var(self.process_vars["Active Logic"], info.get_active_logic_name())
            self._set_var(self.process_vars["Charge Level"], info.get_charge_level_name())
            charge_status = info.get_charge_status_name()
            self._set_var(
                self.process_vars["Stop Reason"],
                info.get_stop_reason_name() if charge_status == "Stopped" else "None",
            )
            self._set_var(
                self.process_vars["Controller Fault"],
                f"0x{info.controller_fault_flags:08X}" if info.controller_fault_flags else "None",
            )
            self._set_var(self.process_vars["Current Limit"],
                f"{info.active_limit_current_c:.2f}" if info.active_limit_current_c else
                ("0.00" if info.controller_inhibit else "---")
            )
            self._set_var(self.process_vars["Actual Charge Voltage"],
                f"{info.total_voltage:.2f}" if info.total_voltage else "---"
            )
            self._set_var(self.process_vars["Actual Charge Current"],
                f"{info.total_current:.2f}" if info.total_current else "---"
            )
            self._set_var(self.process_vars["Charger Max Temp"],
                f"{info.max_temp_dcdc:.1f}" if info.max_temp_dcdc else "---"
            )

            summary = []
            if self._is_monitor_stale(self._last_system_rx):
                summary.append("SYSTEM_INFO timeout")
            snapshot_mismatch = bms is not None and not bms.online and info.charging
            if self._is_monitor_stale(self._last_system_rx):
                pass
            elif snapshot_mismatch:
                summary.append("SNAPSHOT MISMATCH: BMS offline / controller running")
            elif charge_status == "Stopped":
                summary.append("Stopped")
                if info.controller_stop_reason:
                    summary.append(info.get_stop_reason_name())
            elif charge_status == "Blocked":
                summary.append(f"Charging blocked: {info.get_charge_block_reason_name()}")
            elif charge_status == "Derating":
                summary.append(f"Charging, current limited by {info.get_active_logic_name().lower()}")
            else:
                summary.append("Charging")
            self._set_var(self.process_vars["Process Summary"], ", ".join(summary))

    def _clear_traffic_log(self):
        """Clear traffic log"""
        self.traffic.clear()
        self._error_count = 0  # Reset cached error count
        self._refresh_traffic_view()

    def _save_traffic_log(self):
        """Save traffic log"""
        if not self.traffic:
            messagebox.showinfo("Save", "No traffic to save")
            return
        filename = filedialog.asksaveasfilename(defaultextension=".csv",
                                                filetypes=[("CSV", "*.csv"), ("Log", "*.log")])
        if filename:
            with open(filename, "w", newline="", encoding="utf-8") as f:
                writer = csv.writer(f)
                writer.writerow(["Time", "Type", "ID", "DLC", "Data", "Module", "Info"])
                for entry in self.traffic:
                    writer.writerow([entry.time, entry.type, entry.id_hex, entry.dlc,
                                   entry.data_hex, entry.module, entry.info])

    def _on_frame(self, cmd: int, payload: bytes):
        """Handle received frame — queue for batch processing in GUI loop"""
        self._frame_queue.put((cmd, payload))

    def _process_frame(self, cmd: int, payload: bytes):
        """Process frame in main thread"""
        result = self.parser.parse_frame(cmd, payload)
        self._add_traffic("RX", f"{cmd:03X}", len(payload), payload, f"Response: 0x{cmd:02X}")

        if cmd == DebugRsp.ALL_MODULES and isinstance(result, list):
            for mod_data in result:
                self._update_module_from_data(mod_data)
            self._ui_dirty = True
            self._last_module_rx = time.monotonic()

        elif cmd == DebugRsp.MODULE_DATA and isinstance(result, ModuleData):
            self._update_module_from_data(result)
            self._ui_dirty = True

        elif cmd == DebugRsp.BMS_DATA and isinstance(result, BMSData):
            self.bms_data = result
            self._ui_dirty = True
            self._last_bms_rx = time.monotonic()

        elif cmd == DebugRsp.SYSTEM_INFO and isinstance(result, SystemInfo):
            self.system_info = result
            self._ui_dirty = True
            self._last_system_rx = time.monotonic()

        elif cmd == DebugRsp.ERROR:
            err_msg = str(result)
            print(f"[DEBUG] ERROR frame received: {err_msg}")
            
            # Cancel timeout if it exists
            if hasattr(self, '_cfg_read_timeout_id') and self._cfg_read_timeout_id:
                self.root.after_cancel(self._cfg_read_timeout_id)
                self._cfg_read_timeout_id = None
                
            if self._charge_config_built and hasattr(self, '_cfg_status_var'):
                self._cfg_status_var.set(f"❌ Error: {err_msg}")
            messagebox.showerror("MCU Error", f"MCU returned error: {err_msg}")

        elif cmd == DebugRsp.CHARGE_CFG:
            print(f"[DEBUG] CHARGE_CFG received: payload={len(payload)}B, parse_result={type(result).__name__}, tab_built={self._charge_config_built}")
            
            # Cancel timeout if it exists
            if hasattr(self, '_cfg_read_timeout_id') and self._cfg_read_timeout_id:
                self.root.after_cancel(self._cfg_read_timeout_id)
                self._cfg_read_timeout_id = None
                
            if isinstance(result, ChargeCycleConfig):
                self._add_traffic("SYS", "---", len(payload), payload, "Charge config received from MCU")
                if self._charge_config_built:
                    self._load_charge_config_to_ui(result)
                    if hasattr(self, '_cfg_status_var'):
                        self._cfg_status_var.set("✅ Config loaded successfully")
                else:
                    # Tab not yet built — store for later, apply when user opens the tab
                    self._pending_charge_config = result
                messagebox.showinfo("Info", "Charge config loaded from MCU successfully")
            else:
                print(f"[DEBUG] CHARGE_CFG parse FAILED: result={result!r}")
                if self._charge_config_built and hasattr(self, '_cfg_status_var'):
                    self._cfg_status_var.set("❌ Failed to parse config")
                messagebox.showerror("Error", f"Failed to parse charge config: {result}")

    def _update_module_from_data(self, data: ModuleData):
        """Update internal module state from ALL_MODULES/MODULE_DATA telemetry,
        auto-creating a tracked entry the first time the MCU reports a given
        addr+driver.

        MCU flash-persisted config (module_type + source_module_count) is the
        real source of truth for what modules exist -- it auto-registers them
        at boot independently of the PC app (see ChargeCycleConfig_Set()'s
        registration side effect / AUDIT_Findings.md B-21/B-23). If the MCU
        is streaming live telemetry for addr+driver, that module genuinely
        exists right now, whether or not this app session ever clicked "Add"
        for it. Marking it user_added=True here (not the dataclass's False
        default) is what surfaces it on the Control screen table immediately
        -- confirmed with the user 2026-08-29: previously this silently
        tracked the module but hid it from the table until a redundant
        manual "Add" (which could itself NACK as "already exists" against
        the very module the MCU had already told the app about, leaving the
        operator stuck with an empty, unselectable table -- see the
        _add_module() fix immediately above this in git history).

        Exception: a key the user explicitly Removed/Cleared
        (_user_removed_keys) is intentionally NOT auto-recreated here, even
        though the MCU keeps streaming it -- otherwise Remove would look
        broken (module reappearing within ~1s on its own).
        """
        # O(1) lookup using dictionary
        key = (data.addr, data.driver_id)
        if key in self._user_removed_keys:
            return
        idx = self._module_lookup.get(key)
        mod = self.modules.get(idx) if idx is not None else None

        # Auto-create module if not in dictionary
        if mod is None:
            idx = len(self.modules)
            mod = ChargerModule(addr=data.addr, driver=data.driver_id, module_idx=idx, user_added=True)
            self.modules[idx] = mod
            self._module_lookup[key] = idx

        mod.online = data.online
        mod.running = data.running
        mod.state = data.state
        mod.voltage = data.voltage if data.voltage else None
        mod.current = data.current if data.current else None
        mod.current_limit = data.current_limit if data.current_limit else None

        # Charge Graph tab: sample only the currently-selected module, driven
        # by real telemetry arrival (not the GUI tick) -- see
        # _append_charge_graph_sample()'s docstring for why this is safe to
        # do unconditionally here even when the tab has never been opened.
        if idx == self.selected_module_idx:
            self._append_charge_graph_sample(mod.voltage, mod.current)
        mod.temp_dcdc = data.temp_dcdc if data.temp_dcdc else None
        mod.temp_ambient = data.temp_ambient if data.temp_ambient else None
        mod.temp_pfc = data.temp_pfc if data.temp_pfc else None
        mod.ac_phase_a = data.ac_phase_a_voltage if data.ac_phase_a_voltage else None
        mod.ac_phase_b = data.ac_phase_b_voltage if data.ac_phase_b_voltage else None
        mod.ac_phase_c = data.ac_phase_c_voltage if data.ac_phase_c_voltage else None
        mod.pfc_bus_plus = data.pfc_bus_pos_voltage if data.pfc_bus_pos_voltage else None
        mod.pfc_bus_minus = data.pfc_bus_neg_voltage if data.pfc_bus_neg_voltage else None
        mod.input_power = float(data.input_power) if data.input_power else None
        mod.rated_power = data.rated_power if data.rated_power else None
        mod.rated_current = data.rated_current if data.rated_current else None
        mod.alarm_flags = data.alarm_flags
        mod.status_flags = data.alarm_status
        mod.pfc_fault = data.pfc_fault
        mod.tx_count = data.tx_count
        mod.rx_count = data.rx_count
        mod.error_count = data.error_count
        mod.timeout_count = data.timeout_count
        mod.recovery_count = data.recovery_count
        mod.last_update = time.time()

    # =========================================================================
    # CHARGE GRAPH TAB — plain tk.Canvas line chart, deliberately not
    # matplotlib (see AUDIT_Findings.md / this session's lag investigation:
    # the actual lag came from disk I/O on the serial RX thread, not GUI
    # rendering, but a FigureCanvasTkAgg redraw loop would still be the
    # wrong default for a "realtime" chart in an app that just had a
    # performance pass — every redraw here stays a handful of polyline
    # draws on a small point history, throttled to real sample arrival and
    # skipped entirely while the tab isn't visible).
    # =========================================================================

    # Time gap (seconds) beyond which two consecutive samples are drawn as a
    # break in the line rather than joined -- telemetry normally arrives
    # ~1/s, so a >3s gap means the module/BMS genuinely stopped reporting
    # for a while (offline, fault, tab was hidden) and joining across it
    # would misleadingly imply continuous data, the way commercial
    # strip-chart tools (and oscilloscopes) never do.
    CHARGE_GRAPH_GAP_S = 3.0

    def _build_charge_graph_tab(self, parent):
        """Real-time Charge Voltage/Current chart for the selected module."""
        parent.grid_columnconfigure(0, weight=1)
        parent.grid_rowconfigure(1, weight=1)

        header = tk.Frame(parent, bg="#F0F0F0")
        header.grid(row=0, column=0, sticky="ew", padx=10, pady=(10, 4))

        self.lbl_charge_graph_module = tk.Label(
            header, text="Select a module to see live chart",
            font=("Segoe UI", 10, "bold"), bg="#F0F0F0",
        )
        self.lbl_charge_graph_module.pack(side=tk.LEFT)

        # "Now" readout -- the latest V/I values, large and color-matched to
        # the plot lines, right next to the module name. Commercial charging
        # dashboards always lead with this; a technician glancing at the tab
        # shouldn't have to hunt the line's right edge or flip to Monitor.
        self.lbl_charge_graph_now = tk.Label(
            header, text="", font=("Segoe UI", 11, "bold"), bg="#F0F0F0",
        )
        self.lbl_charge_graph_now.pack(side=tk.LEFT, padx=(16, 0))

        ttk.Button(header, text="Clear", command=self._clear_charge_graph, width=8).pack(side=tk.RIGHT)

        canvas_frame = tk.Frame(parent, bg="#FFFFFF", highlightthickness=1, highlightbackground="#B8B8B8")
        canvas_frame.grid(row=1, column=0, sticky="nsew", padx=10, pady=(0, 10))
        canvas_frame.grid_columnconfigure(0, weight=1)
        canvas_frame.grid_rowconfigure(0, weight=1)

        self.charge_graph_canvas = tk.Canvas(canvas_frame, bg="#FFFFFF", highlightthickness=0)
        self.charge_graph_canvas.grid(row=0, column=0, sticky="nsew")
        # Resize -> redraw at the new size. This is the app's only sizing
        # logic for this tab -- no separate DPI/breakpoint handling needed,
        # it just always draws to whatever size the canvas actually has.
        # Debounced resize -> redraw, same after_idle pattern already used
        # by _build_scrollable_page()'s sync_scrollregion(). A live
        # window-resize drag fires <Configure> dozens of times per second;
        # redrawing the full canvas (delete("all") + every line/tick/label)
        # synchronously on every single one of those events is exactly the
        # kind of thing that shows up as visible tearing -- the canvas is
        # mid-repaint again before the previous repaint ever finished.
        # after_idle collapses a whole burst of events into one redraw once
        # Tk actually goes idle.
        self.charge_graph_canvas.bind("<Configure>", self._on_charge_graph_configure)

    def _on_charge_graph_configure(self, _event=None):
        canvas = self.charge_graph_canvas
        if getattr(canvas, "_redraw_scheduled", False):
            return
        canvas._redraw_scheduled = True

        def do_redraw():
            canvas._redraw_scheduled = False
            self._redraw_charge_graph()

        canvas.after_idle(do_redraw)

    def _clear_charge_graph(self):
        self.charge_graph_history.clear()
        self._charge_graph_t0 = None
        self._redraw_charge_graph()

    def _append_charge_graph_sample(self, voltage: Optional[float], current: Optional[float]):
        """Record one (timestamp, V, I) sample for the currently-selected
        module. Called from _update_module_from_data() on real telemetry
        arrival (~1/s, matching the MCU's stream cadence) -- never from the
        60fps GUI tick, so this can't become a new lag source no matter how
        it's later rendered. Safe to call even if the Charge Graph tab has
        never been built (history just accumulates quietly in the deque;
        _redraw_charge_graph() only touches widgets that exist)."""
        if voltage is None and current is None:
            return
        if self._charge_graph_t0 is None:
            self._charge_graph_t0 = time.time()
        self.charge_graph_history.append((time.time(), voltage, current))
        if self._charge_graph_active and self._charge_graph_built:
            self._redraw_charge_graph()

    def _charge_graph_y_limits(self):
        """Fixed Y-axis limits from the module's hardware envelope
        (module_u_max_v / module_i_max_a), not the data's own min/max.

        Commercial charge dashboards never auto-scale the axis to whatever
        the last few samples happened to be -- a rock-steady 80A reading
        would auto-scale to a hair's-width band and *look* like it's
        swinging wildly. A fixed, physically-meaningful scale (0 up to the
        module's actual rated limit) keeps the chart's vertical position
        meaningful and stable for the whole session. Falls back to a
        generic placeholder range only if charge config hasn't loaded yet.
        """
        cfg = self.charge_config
        v_max = cfg.module_u_max_v if cfg is not None and cfg.module_u_max_v > 0 else 100.0
        i_max = cfg.module_i_max_a if cfg is not None and cfg.module_i_max_a > 0 else 100.0
        return 0.0, v_max, 0.0, i_max

    def _redraw_charge_graph(self):
        """Redraw the Charge Graph canvas from self.charge_graph_history.
        No-op if the tab hasn't been built yet, or the canvas has no usable
        size yet (e.g. during initial layout)."""
        canvas = getattr(self, "charge_graph_canvas", None)
        if canvas is None:
            return

        mod = self.modules.get(self.selected_module_idx)
        if hasattr(self, "lbl_charge_graph_module"):
            if mod is None:
                self.lbl_charge_graph_module.configure(text="Select a module to see live chart")
            else:
                self.lbl_charge_graph_module.configure(
                    text=f"{mod.get_driver_name()} 0x{mod.addr:02X} — Charge Voltage / Current"
                )
        if hasattr(self, "lbl_charge_graph_now"):
            last = self.charge_graph_history[-1] if self.charge_graph_history else None
            if last is None or mod is None:
                self.lbl_charge_graph_now.configure(text="")
            else:
                _, lv, la = last
                v_txt = f"{lv:.1f}V" if lv is not None else "--- V"
                a_txt = f"{la:.1f}A" if la is not None else "--- A"
                self.lbl_charge_graph_now.configure(text=f"Now: {v_txt} / {a_txt}")

        canvas.delete("all")
        width = canvas.winfo_width()
        height = canvas.winfo_height()
        if width <= 1 or height <= 1:
            return  # not laid out yet

        history = list(self.charge_graph_history)
        # Extra left/right room for the rotated axis-title text; extra
        # bottom room for the elapsed-time ticks below the legend.
        margin_l, margin_r, margin_t, margin_b = 68, 68, 16, 46
        plot_w = max(1, width - margin_l - margin_r)
        plot_h = max(1, height - margin_t - margin_b)

        if len(history) < 2:
            canvas.create_text(
                width / 2, height / 2,
                text="No data yet" if mod is not None else "Select a module to see live chart",
                fill="#888888", font=("Segoe UI", 10),
            )
            return

        v_min, v_max, i_min, i_max = self._charge_graph_y_limits()

        t0 = self._charge_graph_t0 or history[0][0]
        t1 = history[-1][0]
        t_span = max(0.001, t1 - t0)

        def x_of(t):
            return margin_l + (t - t0) / t_span * plot_w

        def y_of_v(v):
            return margin_t + (1.0 - (v - v_min) / (v_max - v_min)) * plot_h

        def y_of_i(a):
            return margin_t + (1.0 - (a - i_min) / (i_max - i_min)) * plot_h

        # Horizontal gridlines + axis box. 5 bands -> 6 labeled ticks per
        # side (0%, 20%, ..., 100% of the fixed range), not just min/max --
        # a bare 2-tick axis is what read as "empty"/sparse before.
        for i in range(6):
            frac = i / 5.0
            y = margin_t + plot_h * frac
            canvas.create_line(margin_l, y, margin_l + plot_w, y, fill="#EEEEEE")
            v_tick = v_max - (v_max - v_min) * frac
            i_tick = i_max - (i_max - i_min) * frac
            canvas.create_text(margin_l - 8, y, text=f"{v_tick:.0f}", anchor="e", fill="#1565C0", font=("Segoe UI", 8))
            canvas.create_text(margin_l + plot_w + 8, y, text=f"{i_tick:.0f}", anchor="w", fill="#EF6C00", font=("Segoe UI", 8))
        canvas.create_rectangle(margin_l, margin_t, margin_l + plot_w, margin_t + plot_h, outline="#CCCCCC")

        # Rotated axis titles (units live here once, not repeated per tick).
        canvas.create_text(margin_l - 40, margin_t + plot_h / 2, text="Voltage (V)",
                            angle=90, fill="#1565C0", font=("Segoe UI", 9, "bold"))
        canvas.create_text(margin_l + plot_w + 40, margin_t + plot_h / 2, text="Current (A)",
                            angle=270, fill="#EF6C00", font=("Segoe UI", 9, "bold"))

        # Time axis (X): elapsed time (mm:ss) since the session started
        # (first sample since the last Clear/module switch), not
        # wall-clock -- what a technician timing a charge cares about is
        # "12 minutes in", not the clock on the wall.
        for i in range(5):
            frac = i / 4.0
            x = margin_l + plot_w * frac
            canvas.create_line(x, margin_t, x, margin_t + plot_h, fill="#F5F5F5")
            elapsed = t_span * frac
            t_label = f"{int(elapsed // 60):d}:{int(elapsed % 60):02d}"
            anchor = "n" if 0 < i < 4 else ("nw" if i == 0 else "ne")
            canvas.create_text(x, margin_t + plot_h + 6, text=t_label, anchor=anchor,
                                fill="#666666", font=("Segoe UI", 8))

        # Voltage/current lines, split into separate segments across any
        # gap wider than CHARGE_GRAPH_GAP_S so a real telemetry dropout
        # reads as a break, not a smoothed-over straight line.
        def draw_segments(value_index, color):
            segment = []
            last_t = None
            for t, v, a in history:
                val = v if value_index == 1 else a
                gap = last_t is not None and (t - last_t) > self.CHARGE_GRAPH_GAP_S
                if val is None or gap:
                    if len(segment) >= 4:
                        canvas.create_line(*segment, fill=color, width=2, capstyle=tk.ROUND, joinstyle=tk.ROUND)
                    segment = []
                if val is not None:
                    y = y_of_v(val) if value_index == 1 else y_of_i(val)
                    segment.extend((x_of(t), y))
                last_t = t
            if len(segment) >= 4:
                canvas.create_line(*segment, fill=color, width=2, capstyle=tk.ROUND, joinstyle=tk.ROUND)

        draw_segments(1, "#1565C0")  # Voltage
        draw_segments(2, "#EF6C00")  # Current

        # Legend.
        canvas.create_line(margin_l, height - 12, margin_l + 20, height - 12, fill="#1565C0", width=2)
        canvas.create_text(margin_l + 26, height - 12, text="Voltage", anchor="w", fill="#1565C0", font=("Segoe UI", 8))
        canvas.create_line(margin_l + 90, height - 12, margin_l + 110, height - 12, fill="#EF6C00", width=2)
        canvas.create_text(margin_l + 116, height - 12, text="Current", anchor="w", fill="#EF6C00", font=("Segoe UI", 8))

    def _check_alarms(self):
        """Rebuild active alarm list from the current module snapshots."""
        new_alarms: List[AlarmInfo] = []
        new_keys = set()
        now = self._last_timestamp or time.strftime("%H:%M:%S")

        for mod in self.modules.values():
            module_name = f"0x{mod.addr:02X}"
            alarm_names = mod.get_alarm_names()
            for name in alarm_names:
                key = (module_name, name)
                new_keys.add(key)
                new_alarms.append(AlarmInfo(
                    num=0,
                    timestamp=now,
                    module=module_name,
                    level="FAULT",
                    message=name,
                ))

        for idx, alarm in enumerate(new_alarms, start=1):
            alarm.num = idx

        added_keys = new_keys - self.active_alarm_keys
        cleared_keys = self.active_alarm_keys - new_keys

        # Skip UI rebuild if nothing changed
        if not added_keys and not cleared_keys:
            return

        for module_name, name in sorted(added_keys):
            self._add_traffic("WARN", module_name.replace("0x", ""), 0, b"", f"ALARM: {name}", "WARN")

        for module_name, name in sorted(cleared_keys):
            self._add_traffic("SYS", module_name.replace("0x", ""), 0, b"", f"ALARM CLEARED: {name}", "SYS")

        self.active_alarm_keys = new_keys
        self.alarms = new_alarms
        self.next_alarm_num = len(self.alarms) + 1
        self._update_alarm_list()

    def _on_log(self, msg: str):
        """Handle log message — capped to prevent memory leak"""
        if len(self.log_entries) < 2000:
            self.log_entries.append(msg)
        # Print serial-level RX/TX log to console for diagnostics
        if "0x97" in msg or "GET_CHARGE" in msg or "CHARGE_CFG" in msg or "Bad CRC" in msg:
            print(f"[SERIAL] {msg}")

    def _get_timestamp(self):
        """Get current timestamp"""
        return time.strftime("%H:%M:%S")


# =============================================================================
# ENTRY POINT
# =============================================================================

if __name__ == "__main__":
    root = tk.Tk()
    app = ChargerDebugApp(root)
    root.mainloop()
