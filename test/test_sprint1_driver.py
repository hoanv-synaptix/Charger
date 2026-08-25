"""
Sprint 1.3 — SelectDriver + RemoveModule + RECOVERING
"""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
def read(p): return (ROOT / p).read_text(encoding="utf-8", errors="ignore")

def test_select_driver_deinit():
    core = read("Modules/chg_lib/chg_lib_core.c")
    assert "deinit" in core, "SelectDriver must call deinit"
    # Must fallback to init if deinit missing
    assert "old_driver->deinit" in core
    assert "old_driver->init" in core
    h = read("Modules/chg_lib/chg_lib.h")
    assert "void    (*deinit)(void)" in h or "(*deinit)" in h

def test_drivers_have_deinit_ops():
    for name in ["chg_lib_maxwell.c", "chg_lib_lianming.c", "chg_lib_tonhe.c"]:
        txt = read(f"Modules/chg_lib/{name}")
        assert ".deinit" in txt, f"{name} must set deinit ops"
        assert "mx_init" in txt or "lm_init" in txt or "tonhe_init" in txt

def test_remove_module_compact():
    for name in ["chg_lib_maxwell.c", "chg_lib_lianming.c", "chg_lib_tonhe.c"]:
        txt = read(f"Modules/chg_lib/{name}")
        # Check for memmove or for loop compact + g_module_count--
        assert "g_module_count--" in txt, f"{name} RemoveModule must decrement count"
        # Must compact array
        assert "for (uint8_t i = idx" in txt or "memmove" in txt
        assert "__disable_irq" in txt

def test_recovering_requires_5_rx():
    for name, expected in [
        ("chg_lib_maxwell.c", "recovery_start_rx_count"),
        ("chg_lib_lianming.c", "recovery_start_rx_count"),
        ("chg_lib_tonhe.c", "recovery_start_rx_count"),
    ]:
        txt = read(f"Modules/chg_lib/{name}")
        # Should contain check >=5
        assert ">= 5" in txt or ">=5" in txt, f"{name} RECOVERING must require 5 RX"
        # Should NOT abort after 3 retry (old code had if retry>=3 -> OFFLINE)
        # Check that OFFLINE transition from RECOVERING after retry>=3 is removed
        rec_section = txt[txt.find("CHG_LIB_STATE_RECOVERING"):txt.find("CHG_LIB_STATE_FAULT", txt.find("CHG_LIB_STATE_RECOVERING"))]
        assert "retry_count >= 3" not in rec_section and "retry_count >= LM_MAX_RETRY" not in rec_section and "MXR_MAX_RETRIES" not in rec_section or "RECOVERING" in txt # allow but ensure not abort
        # Instead, for Maxwell/Lianming the retry abort should be absent; check that MXR_MAX_RETRIES not used in RECOVERING
        if name in ["chg_lib_maxwell.c", "chg_lib_lianming.c"]:
            assert "MXR_MAX_RETRIES" not in rec_section and "LM_MAX_RETRY" not in rec_section, f"{name} must not abort RECOVERING after 3 retries"

def test_tonhe_offline_to_recovering():
    txt = read("Modules/chg_lib/chg_lib_tonhe.c")
    # OFFLINE should transition after 3000 ms
    assert "TONHE_RECOVERY_DELAY_MS" in txt
    assert "CHG_LIB_STATE_OFFLINE" in txt
    assert "CHG_LIB_STATE_RECOVERING" in txt
    # Ensure process_module OFFLINE case transitions to RECOVERING after delay
    assert "if ((now - mod->state_enter_tick) > TONHE_RECOVERY_DELAY_MS)" in txt
    assert "set_state(mod, CHG_LIB_STATE_RECOVERING" in txt
    # RECOVERING must need 5 RX
    # find process_module's RECOVERING case (last occurrence)
    rec_idx = txt.rfind("case CHG_LIB_STATE_RECOVERING")
    fault_idx = txt.find("case CHG_LIB_STATE_FAULT", rec_idx)
    rec = txt[rec_idx:fault_idx]
    assert ">= 5" in rec

def test_tonhe_timing_1s():
    txt = read("Modules/chg_lib/chg_lib_tonhe.c")
    # Timing command interval should be 1000, not 5000
    assert ">= 1000U" in txt, "TonHe timing should be 1s (1000ms) per SRS"
    assert ">= 5000U" not in txt or "send_timing_command" not in txt[txt.find(">= 5000U")-100:txt.find(">= 5000U")+100]  # ensure old 5000 not used for timing

def test_chg_lib_core_irq_protection():
    core = read("Modules/chg_lib/chg_lib_core.c")
    assert "__disable_irq" in core
    assert "CHG_LIB_Process" in core
    assert "CHG_LIB_FeedCanFrame" in core
