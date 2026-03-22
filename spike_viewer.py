import json
import math
import os
import queue
import threading
import tkinter as tk
from dataclasses import dataclass
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import Optional
import xml.etree.ElementTree as ET

os.environ.setdefault("MPLCONFIGDIR", str(Path.cwd() / ".mplconfig"))

import matplotlib
matplotlib.use("TkAgg")
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure
import numpy as np


INTAN_MAGIC = 0xC6912702
INTAN_UV_PER_BIT = 0.195


@dataclass
class ChannelInfo:
    index: int
    label: str
    electrode_id: int
    x: float
    y: float


@dataclass
class RecordingData:
    base_dir: Path
    info_path: Path
    amplifier_path: Path
    time_path: Optional[Path]
    sample_rate: float
    sample_count: int
    duration_seconds: float
    channel_count: int
    channels: list[ChannelInfo]
    amplifier: np.memmap
    time_data: Optional[np.memmap]


def parse_intan_header(path: Path) -> dict:
    with path.open("rb") as handle:
        header = handle.read(32)
    if len(header) < 12:
        raise ValueError("info.rhd is too short to be a valid Intan header.")
    magic = int.from_bytes(header[0:4], "little")
    if magic != INTAN_MAGIC:
        raise ValueError("info.rhd does not look like an Intan RHD file.")
    major = int.from_bytes(header[4:6], "little", signed=True)
    minor = int.from_bytes(header[6:8], "little", signed=True)
    sample_rate = np.frombuffer(header[8:12], dtype="<f4")[0].item()
    return {"version": f"{major}.{minor}", "sample_rate": float(sample_rate)}


def find_first(base_dir: Path, patterns: list[str]) -> Optional[Path]:
    for pattern in patterns:
        matches = sorted(base_dir.glob(pattern))
        if matches:
            return matches[0]
    return None


def parse_data_config(base_dir: Path) -> dict:
    config_path = find_first(base_dir, ["*data*.json", "*Data*.json"])
    if not config_path:
        return {}
    with config_path.open("r", encoding="utf-8") as handle:
        entries = json.load(handle)
    result = {"config_path": config_path}
    for entry in entries:
        if entry.get("filepath") == "amplifier.dat":
            result["channel_count"] = int(entry.get("channel_count", 0))
        if entry.get("name") == "master" and entry.get("filepath"):
            result["time_path"] = base_dir / entry["filepath"]
    return result


def parse_settings_xml(path: Path) -> tuple[Optional[float], list[str]]:
    if not path.exists():
        return None, []
    root = ET.parse(path).getroot()
    sample_rate = root.attrib.get("SampleRateHertz")
    labels: list[str] = []
    for signal_group in root.findall("SignalGroup"):
        for channel in signal_group.findall("Channel"):
            if channel.attrib.get("Enabled", "True") != "True":
                continue
            labels.append(
                channel.attrib.get("CustomChannelName")
                or channel.attrib.get("NativeChannelName")
                or f"Ch {len(labels) + 1}"
            )
    parsed_rate = float(sample_rate) if sample_rate else None
    return parsed_rate, labels


def parse_electrode_cfg(path: Path, channel_count: int, labels: list[str]) -> list[ChannelInfo]:
    channels: list[ChannelInfo] = []
    if not path.exists():
        return [
            ChannelInfo(index=i, label=labels[i] if i < len(labels) else f"Ch {i + 1}", electrode_id=i + 1, x=float(i), y=0.0)
            for i in range(channel_count)
        ]

    lines = [line.strip() for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    for line in lines[1:]:
        clean = line.replace(",", " ")
        parts = clean.split()
        if len(parts) < 4:
            continue
        try:
            electrode_id = int(parts[0])
            channel_number = int(parts[1])
            x_coord = float(parts[2])
            y_coord = float(parts[3])
        except ValueError:
            continue
        index = channel_number - 1
        label = labels[index] if index < len(labels) else f"Ch {channel_number}"
        channels.append(
            ChannelInfo(
                index=index,
                label=label,
                electrode_id=electrode_id,
                x=x_coord,
                y=y_coord,
            )
        )

    if not channels:
        channels = [
            ChannelInfo(index=i, label=labels[i] if i < len(labels) else f"Ch {i + 1}", electrode_id=i + 1, x=float(i), y=0.0)
            for i in range(channel_count)
        ]

    channels.sort(key=lambda item: item.index)
    return channels


def load_recording(info_path: Path) -> RecordingData:
    base_dir = info_path.parent
    header = parse_intan_header(info_path)
    config = parse_data_config(base_dir)

    amplifier_path = base_dir / "amplifier.dat"
    if not amplifier_path.exists():
        raise FileNotFoundError("amplifier.dat was not found next to info.rhd.")

    settings_path = base_dir / "settings.xml"
    settings_rate, labels = parse_settings_xml(settings_path)

    channel_count = int(config.get("channel_count") or len(labels) or 32)
    sample_rate = settings_rate or header["sample_rate"]
    if not sample_rate:
        raise ValueError("Unable to determine the sample rate from info.rhd or settings.xml.")

    raw = np.memmap(amplifier_path, dtype=np.int16, mode="r")
    if raw.size % channel_count != 0:
        raise ValueError("amplifier.dat size is not divisible by the detected channel count.")
    amplifier = raw.reshape(-1, channel_count)

    time_path = config.get("time_path") or find_first(base_dir, ["time.dat"])
    time_data = None
    if time_path and Path(time_path).exists():
        time_data = np.memmap(time_path, dtype=np.int32, mode="r")

    channels = parse_electrode_cfg(base_dir / "electrode.cfg", channel_count, labels)
    sample_count = int(amplifier.shape[0])
    duration_seconds = sample_count / sample_rate

    return RecordingData(
        base_dir=base_dir,
        info_path=info_path,
        amplifier_path=amplifier_path,
        time_path=Path(time_path) if time_path else None,
        sample_rate=sample_rate,
        sample_count=sample_count,
        duration_seconds=duration_seconds,
        channel_count=channel_count,
        channels=channels,
        amplifier=amplifier,
        time_data=time_data,
    )


def downsample_trace(signal_uv: np.ndarray, target_points: int = 2500) -> tuple[np.ndarray, np.ndarray]:
    if signal_uv.size <= target_points:
        x = np.linspace(0.0, 1.0, signal_uv.size, endpoint=False)
        return x, signal_uv
    step = int(math.ceil(signal_uv.size / target_points))
    trimmed = signal_uv[: signal_uv.size - (signal_uv.size % step)]
    if trimmed.size == 0:
        x = np.linspace(0.0, 1.0, signal_uv.size, endpoint=False)
        return x, signal_uv
    reshaped = trimmed.reshape(-1, step)
    mins = reshaped.min(axis=1)
    maxs = reshaped.max(axis=1)
    envelope = np.empty(mins.size * 2, dtype=np.float32)
    envelope[0::2] = mins
    envelope[1::2] = maxs
    x = np.repeat(np.arange(mins.size), 2).astype(np.float32)
    x /= max(mins.size, 1)
    return x, envelope


def estimate_event_thresholds(recording: RecordingData, window_samples: int = 6000, sample_windows: int = 10) -> np.ndarray:
    if recording.sample_count <= 0:
        return np.full(recording.channel_count, -20.0, dtype=np.float32)

    window_samples = max(256, min(window_samples, recording.sample_count))
    max_start = max(recording.sample_count - window_samples, 0)
    starts = np.linspace(0, max_start, num=max(1, sample_windows), dtype=int)
    noise_estimates: list[np.ndarray] = []

    for start in starts:
        end = start + window_samples
        chunk = np.asarray(recording.amplifier[start:end, :], dtype=np.float32) * INTAN_UV_PER_BIT
        centered = chunk - np.median(chunk, axis=0, keepdims=True)
        mad = np.median(np.abs(centered), axis=0)
        sigma = mad / 0.6745
        noise_estimates.append(sigma.astype(np.float32))

    sigma = np.median(np.stack(noise_estimates, axis=0), axis=0)
    sigma = np.maximum(sigma, 2.5)
    return (-4.0 * sigma).astype(np.float32)


class SpikeViewerApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("Spike Sort Viewer")
        self.root.geometry("1460x900")

        self.recording: Optional[RecordingData] = None
        self.selected_channel: Optional[int] = None
        self.window_seconds = tk.DoubleVar(value=0.02)
        self.time_seconds = tk.DoubleVar(value=0.0)
        self.voltage_scale = tk.DoubleVar(value=1.0)
        self.overview_mode = tk.StringVar(value="Activity")
        self.status_text = tk.StringVar(value="Load an info.rhd to begin.")
        self.summary_text = tk.StringVar(value="")
        self.stats_text = tk.StringVar(value="Select a channel")
        self.heatmap_queue: queue.Queue = queue.Queue()
        self.heatmap_matrix: Optional[np.ndarray] = None
        self.heatmap_times: Optional[np.ndarray] = None
        self.heatmap_order: list[int] = []
        self.motion_matrix: Optional[np.ndarray] = None
        self.event_matrix: Optional[np.ndarray] = None
        self.rms_matrix: Optional[np.ndarray] = None
        self.ptp_matrix: Optional[np.ndarray] = None
        self.population_activity: Optional[np.ndarray] = None
        self.population_events: Optional[np.ndarray] = None
        self.common_mode_series: Optional[np.ndarray] = None
        self.event_thresholds: Optional[np.ndarray] = None

        self.channel_listbox: Optional[tk.Listbox] = None
        self.trace_figure: Optional[Figure] = None
        self.trace_axis = None
        self.trace_canvas: Optional[FigureCanvasTkAgg] = None
        self.stats_panel: Optional[ttk.Frame] = None
        self.stats_label: Optional[ttk.Label] = None
        self.detail_figure: Optional[Figure] = None
        self.detail_canvas: Optional[FigureCanvasTkAgg] = None
        self.detail_axis = None
        self.heatmap_figure: Optional[Figure] = None
        self.heatmap_axis = None
        self.heatmap_canvas: Optional[FigureCanvasTkAgg] = None

        self._build_ui()
        self.root.after(200, self._poll_heatmap_queue)

    def _build_ui(self) -> None:
        outer = ttk.Frame(self.root, padding=4)
        outer.pack(fill=tk.BOTH, expand=True)

        control_bar = ttk.Frame(outer)
        control_bar.pack(fill=tk.X, pady=(0, 3))

        ttk.Button(control_bar, text="Load info.rhd", command=self.open_file).pack(side=tk.LEFT)
        ttk.Label(control_bar, textvariable=self.summary_text).pack(side=tk.LEFT, padx=12)

        ttk.Label(control_bar, text="Window (s)").pack(side=tk.LEFT, padx=(24, 4))
        window_box = ttk.Spinbox(
            control_bar,
            from_=0.001,
            to=2.0,
            increment=0.001,
            textvariable=self.window_seconds,
            width=8,
            command=self.refresh_views,
        )
        window_box.pack(side=tk.LEFT)
        window_box.bind("<Return>", lambda _event: self.refresh_views())

        ttk.Label(control_bar, text="Time (s)").pack(side=tk.LEFT, padx=(16, 4))
        time_box = ttk.Entry(control_bar, textvariable=self.time_seconds, width=10)
        time_box.pack(side=tk.LEFT)
        time_box.bind("<Return>", lambda _event: self.on_time_entry())

        ttk.Button(control_bar, text="Jump", command=self.on_time_entry).pack(side=tk.LEFT, padx=(4, 0))

        ttk.Label(control_bar, text="Scale").pack(side=tk.LEFT, padx=(16, 4))
        scale_box = ttk.Spinbox(
            control_bar,
            from_=0.1,
            to=20.0,
            increment=0.1,
            textvariable=self.voltage_scale,
            width=6,
            command=self.refresh_views,
        )
        scale_box.pack(side=tk.LEFT)
        scale_box.bind("<Return>", lambda _event: self.refresh_views())

        self.time_slider = tk.Scale(
            outer,
            orient=tk.HORIZONTAL,
            from_=0.0,
            to=1.0,
            resolution=0.01,
            showvalue=False,
            command=self.on_slider_move,
        )
        self.time_slider.pack(fill=tk.X, pady=(0, 4))

        body = ttk.Panedwindow(outer, orient=tk.HORIZONTAL)
        body.pack(fill=tk.BOTH, expand=True)

        sidebar = ttk.Frame(body, width=180)
        body.add(sidebar, weight=0)

        ttk.Label(sidebar, text="Channels").pack(anchor="w")
        self.channel_listbox = tk.Listbox(sidebar, exportselection=False, height=32)
        self.channel_listbox.pack(fill=tk.BOTH, expand=True, pady=(6, 0))
        self.channel_listbox.bind("<<ListboxSelect>>", self.on_channel_select)

        ttk.Label(sidebar, text="Overview").pack(anchor="w", pady=(10, 2))
        overview_picker = ttk.Combobox(
            sidebar,
            textvariable=self.overview_mode,
            values=["Activity", "Events", "RMS", "Peak-to-Peak", "Population", "Motion"],
            state="readonly",
        )
        overview_picker.pack(fill=tk.X)
        overview_picker.bind("<<ComboboxSelected>>", lambda _event: self.refresh_views())

        ttk.Label(sidebar, textvariable=self.status_text, wraplength=190, justify=tk.LEFT).pack(anchor="w", pady=(10, 0))

        content = ttk.Frame(body)
        body.add(content, weight=1)

        vertical_split = tk.PanedWindow(
            content,
            orient=tk.VERTICAL,
            sashwidth=8,
            sashrelief=tk.RAISED,
            bd=0,
            opaqueresize=True,
            bg="#8f8f8f",
        )
        vertical_split.pack(fill=tk.BOTH, expand=True)

        top_frame = ttk.Frame(vertical_split)
        lower_host = ttk.Frame(vertical_split)
        vertical_split.add(top_frame)
        vertical_split.add(lower_host)

        lower = ttk.Panedwindow(lower_host, orient=tk.HORIZONTAL)
        lower.pack(fill=tk.BOTH, expand=True, pady=(2, 0))

        self.trace_figure = Figure(figsize=(8, 2.8), dpi=160)
        self.trace_axis = self.trace_figure.add_subplot(111)
        self.trace_canvas = FigureCanvasTkAgg(self.trace_figure, master=top_frame)
        trace_widget = self.trace_canvas.get_tk_widget()
        trace_widget.pack(fill=tk.BOTH, expand=True)

        self.stats_panel = ttk.Frame(lower, padding=8, relief=tk.GROOVE)
        self.stats_label = tk.Label(
            self.stats_panel,
            textvariable=self.stats_text,
            justify=tk.LEFT,
            anchor="nw",
            bg=self.root.cget("bg"),
            font=("TkDefaultFont", 11),
        )
        self.stats_label.pack(fill=tk.BOTH, expand=True)
        lower.add(self.stats_panel, weight=1)

        self.detail_figure = Figure(figsize=(5.2, 6), dpi=160)
        self.detail_axis = self.detail_figure.add_subplot(111)
        self.detail_canvas = FigureCanvasTkAgg(self.detail_figure, master=lower)
        lower.add(self.detail_canvas.get_tk_widget(), weight=1)

        self.heatmap_figure = Figure(figsize=(7, 4.2), dpi=160)
        self.heatmap_axis = self.heatmap_figure.add_subplot(111)
        self.heatmap_canvas = FigureCanvasTkAgg(self.heatmap_figure, master=lower)
        self.heatmap_canvas.mpl_connect("button_press_event", self.on_heatmap_click)
        lower.add(self.heatmap_canvas.get_tk_widget(), weight=1)

        def initialize_vertical_split() -> None:
            total_height = vertical_split.winfo_height()
            if total_height <= 1:
                self.root.after(50, initialize_vertical_split)
                return
            top_height = max(int(total_height * 0.78), 220)
            try:
                vertical_split.sash_place(0, 0, top_height)
            except tk.TclError:
                self.root.after(50, initialize_vertical_split)

        self.root.after(80, initialize_vertical_split)
        self._draw_empty()

    def open_file(self) -> None:
        selected = filedialog.askopenfilename(
            title="Select info.rhd",
            filetypes=[("Intan header", "*.rhd"), ("All files", "*.*")],
            initialdir=str(Path.cwd()),
        )
        if not selected:
            return
        self.load_info(Path(selected))

    def load_info(self, info_path: Path) -> None:
        try:
            recording = load_recording(info_path)
        except Exception as exc:
            messagebox.showerror("Load failed", str(exc))
            return

        self.recording = recording
        self.selected_channel = None
        self.heatmap_matrix = None
        self.heatmap_times = None
        self.heatmap_order = []
        self.motion_matrix = None
        self.event_matrix = None
        self.rms_matrix = None
        self.ptp_matrix = None
        self.population_activity = None
        self.population_events = None
        self.common_mode_series = None
        self.event_thresholds = None
        self.populate_channel_list()

        max_time = max(recording.duration_seconds - self.window_seconds.get(), 0.0)
        self.time_slider.configure(to=max_time)
        self.time_slider.set(0.0)
        self.time_seconds.set(0.0)

        self.summary_text.set(
            f"{recording.info_path.name} | {recording.channel_count} channels | "
            f"{recording.sample_rate:.0f} Hz | {recording.duration_seconds / 60:.1f} min"
        )
        self.status_text.set("Loaded recording. Computing activity heatmap in the background.")
        self.refresh_views()
        self.start_heatmap_worker()

    def populate_channel_list(self) -> None:
        if not self.channel_listbox or not self.recording:
            return
        self.channel_listbox.delete(0, tk.END)
        for channel in self.recording.channels:
            self.channel_listbox.insert(
                tk.END,
                f"{channel.electrode_id:02d} | {channel.label} | ({channel.x:g}, {channel.y:g})",
            )
        self.channel_listbox.selection_clear(0, tk.END)

    def on_channel_select(self, _event=None) -> None:
        if not self.channel_listbox:
            return
        selection = self.channel_listbox.curselection()
        if not selection:
            self.selected_channel = None
            self.refresh_views()
            return
        self.selected_channel = int(selection[0])
        self.refresh_views()

    def on_slider_move(self, value: str) -> None:
        try:
            self.time_seconds.set(float(value))
        except ValueError:
            return
        self.refresh_views()

    def on_time_entry(self) -> None:
        if not self.recording:
            return
        start_time = min(max(self.time_seconds.get(), 0.0), max(self.recording.duration_seconds - self.window_seconds.get(), 0.0))
        self.time_seconds.set(start_time)
        self.time_slider.set(start_time)
        self.refresh_views()

    def refresh_views(self) -> None:
        if not self.recording:
            self._draw_empty()
            return
        max_time = max(self.recording.duration_seconds - self.window_seconds.get(), 0.0)
        self.time_slider.configure(to=max_time)
        start_time = min(max(self.time_seconds.get(), 0.0), max_time)
        self.time_seconds.set(start_time)
        self._draw_trace(start_time)
        self._update_stats_panel(start_time)
        self._draw_detail(start_time)
        self._draw_heatmap()

    def _draw_empty(self) -> None:
        for axis in [self.trace_axis, self.heatmap_axis, self.detail_axis]:
            axis.clear()
            axis.set_axis_off()
        self._remove_overview_colorbar()
        self.trace_canvas.draw_idle()
        self.heatmap_canvas.draw_idle()
        self.detail_canvas.draw_idle()
        self.stats_text.set("Select a channel")

    def _time_window_indices(self, start_time: float) -> tuple[int, int]:
        assert self.recording is not None
        start = int(start_time * self.recording.sample_rate)
        width = max(int(self.window_seconds.get() * self.recording.sample_rate), 1)
        end = min(start + width, self.recording.sample_count)
        return start, end

    def _extract_channel_window(self, channel_index: int, start: int, end: int) -> np.ndarray:
        assert self.recording is not None
        return np.asarray(self.recording.amplifier[start:end, channel_index], dtype=np.float32) * INTAN_UV_PER_BIT

    def _channel_color(self, channel_index: int):
        assert self.recording is not None
        cmap = matplotlib.colormaps["gist_rainbow"].resampled(self.recording.channel_count)
        return cmap(channel_index)

    def _draw_trace(self, start_time: float) -> None:
        assert self.recording is not None
        start, end = self._time_window_indices(start_time)
        all_signals = np.asarray(self.recording.amplifier[start:end, :], dtype=np.float32) * INTAN_UV_PER_BIT
        scale = max(self.voltage_scale.get(), 1e-6)
        unscaled_ptp = np.ptp(all_signals, axis=0)
        spacing = max(float(np.percentile(unscaled_ptp, 75) * 1.2), 90.0)

        self.trace_axis.clear()
        for idx, channel in enumerate(self.recording.channels):
            x_norm, y = downsample_trace(all_signals[:, channel.index] * scale, target_points=1800)
            time_axis = start_time + x_norm * self.window_seconds.get()
            offset = (self.recording.channel_count - 1 - idx) * spacing
            is_selected = self.selected_channel == idx
            line_width = 1.0 if is_selected else 0.7
            alpha = 1.0 if is_selected else 0.8
            self.trace_axis.plot(time_axis, y + offset, color=self._channel_color(idx), linewidth=line_width, alpha=alpha)

        self.trace_axis.set_xticks([])
        self.trace_axis.set_yticks([])
        for spine in self.trace_axis.spines.values():
            spine.set_visible(False)
        self.trace_axis.margins(x=0, y=0.0)
        self.trace_figure.subplots_adjust(left=0.01, right=0.995, top=0.92, bottom=0.03)
        self.trace_canvas.draw_idle()

        self.status_text.set(
            f"Showing all channels from {start_time:.4f}s to {start_time + self.window_seconds.get():.4f}s."
        )

    def _draw_heatmap(self) -> None:
        self.heatmap_axis.clear()
        if self.heatmap_times is None or not self.recording:
            self.heatmap_axis.set_axis_off()
            self._remove_overview_colorbar()
            self.heatmap_canvas.draw_idle()
            return

        mode = self.overview_mode.get()

        if mode == "Motion":
            self._draw_motion_overview()
            return

        if mode == "Population":
            self._draw_population_overview()
            return

        heatmap_source = {
            "Activity": (self.heatmap_matrix, "magma", "activity"),
            "Events": (self.event_matrix, "inferno", "events/bin"),
            "RMS": (self.rms_matrix, "viridis", "uV"),
            "Peak-to-Peak": (self.ptp_matrix, "plasma", "uV"),
        }.get(mode)

        if heatmap_source is None:
            self.heatmap_axis.set_axis_off()
            self._remove_overview_colorbar()
            self.heatmap_canvas.draw_idle()
            return

        matrix, cmap_name, label = heatmap_source
        self._draw_matrix_overview(matrix, cmap_name, label)

    def _draw_matrix_overview(self, matrix: Optional[np.ndarray], cmap_name: str, label: str) -> None:
        if matrix is None or self.heatmap_times is None:
            self.heatmap_axis.set_axis_off()
            self._remove_overview_colorbar()
            self.heatmap_canvas.draw_idle()
            return

        extent = [self.heatmap_times[0], self.heatmap_times[-1], -0.5, len(self.heatmap_order) - 0.5]
        image = self.heatmap_axis.imshow(
            matrix,
            aspect="auto",
            origin="lower",
            cmap=cmap_name,
            extent=extent,
        )
        self.heatmap_axis.set_xticks([])
        self.heatmap_axis.set_yticks([])
        self._update_overview_colorbar(image, label)

        selected_row = self.heatmap_order.index(self.selected_channel) if self.selected_channel is not None and self.selected_channel in self.heatmap_order else None
        if selected_row is not None:
            self.heatmap_axis.axhline(selected_row, color="cyan", linewidth=0.8, alpha=0.7)
        self.heatmap_axis.axvline(self.time_seconds.get(), color="white", linewidth=1.0, alpha=0.9)
        self.heatmap_axis.margins(x=0, y=0)
        for spine in self.heatmap_axis.spines.values():
            spine.set_visible(False)
        self.heatmap_figure.subplots_adjust(left=0.01, right=0.93, top=0.92, bottom=0.02)
        self.heatmap_canvas.draw_idle()

    def _draw_motion_overview(self) -> None:
        if self.motion_matrix is None or self.heatmap_times is None:
            self.heatmap_axis.set_axis_off()
            self._remove_overview_colorbar()
            self.heatmap_canvas.draw_idle()
            return

        x_shift = self.motion_matrix[0]
        y_shift = self.motion_matrix[1]
        drift = self.motion_matrix[2]
        max_abs = max(float(np.max(np.abs(self.motion_matrix))), 1e-6)

        self.heatmap_axis.plot(self.heatmap_times, x_shift, color="#38bdf8", linewidth=1.0, alpha=0.95)
        self.heatmap_axis.plot(self.heatmap_times, y_shift, color="#f97316", linewidth=1.0, alpha=0.95)
        self.heatmap_axis.plot(self.heatmap_times, drift, color="#22c55e", linewidth=1.0, alpha=0.9)
        self.heatmap_axis.axhline(0.0, color="#999999", linewidth=0.6, alpha=0.5)
        self.heatmap_axis.axvline(self.time_seconds.get(), color="white", linewidth=1.0, alpha=0.9)
        self.heatmap_axis.set_xlim(self.heatmap_times[0], self.heatmap_times[-1])
        self.heatmap_axis.set_ylim(-max_abs * 1.1, max_abs * 1.1)
        self.heatmap_axis.set_xticks([])
        self.heatmap_axis.set_yticks([])
        self._remove_overview_colorbar()
        for spine in self.heatmap_axis.spines.values():
            spine.set_visible(False)
        self.heatmap_axis.text(0.02, 0.96, "x", color="#38bdf8", transform=self.heatmap_axis.transAxes, ha="left", va="top", fontsize=9)
        self.heatmap_axis.text(0.10, 0.96, "y", color="#f97316", transform=self.heatmap_axis.transAxes, ha="left", va="top", fontsize=9)
        self.heatmap_axis.text(0.18, 0.96, "drift", color="#22c55e", transform=self.heatmap_axis.transAxes, ha="left", va="top", fontsize=9)
        self.heatmap_figure.subplots_adjust(left=0.01, right=0.99, top=0.96, bottom=0.06)
        self.heatmap_canvas.draw_idle()

    def _draw_population_overview(self) -> None:
        if (
            self.heatmap_times is None
            or self.population_activity is None
            or self.population_events is None
            or self.common_mode_series is None
        ):
            self.heatmap_axis.set_axis_off()
            self._remove_overview_colorbar()
            self.heatmap_canvas.draw_idle()
            return

        def normalize(series: np.ndarray) -> np.ndarray:
            centered = series - np.median(series)
            scale = max(float(np.percentile(np.abs(centered), 95)), 1e-6)
            return centered / scale

        activity_line = normalize(self.population_activity)
        event_line = normalize(self.population_events.astype(np.float32))
        common_line = normalize(self.common_mode_series)

        self.heatmap_axis.plot(self.heatmap_times, activity_line, color="#f59e0b", linewidth=1.0, alpha=0.95)
        self.heatmap_axis.plot(self.heatmap_times, event_line, color="#ef4444", linewidth=1.0, alpha=0.95)
        self.heatmap_axis.plot(self.heatmap_times, common_line, color="#38bdf8", linewidth=1.0, alpha=0.95)
        self.heatmap_axis.axhline(0.0, color="#999999", linewidth=0.6, alpha=0.5)
        self.heatmap_axis.axvline(self.time_seconds.get(), color="white", linewidth=1.0, alpha=0.9)
        self.heatmap_axis.set_xlim(self.heatmap_times[0], self.heatmap_times[-1])
        self.heatmap_axis.set_xticks([])
        self.heatmap_axis.set_yticks([])
        self._remove_overview_colorbar()
        for spine in self.heatmap_axis.spines.values():
            spine.set_visible(False)
        self.heatmap_axis.text(0.02, 0.96, "activity", color="#f59e0b", transform=self.heatmap_axis.transAxes, ha="left", va="top", fontsize=9)
        self.heatmap_axis.text(0.22, 0.96, "events", color="#ef4444", transform=self.heatmap_axis.transAxes, ha="left", va="top", fontsize=9)
        self.heatmap_axis.text(0.39, 0.96, "common", color="#38bdf8", transform=self.heatmap_axis.transAxes, ha="left", va="top", fontsize=9)
        self.heatmap_figure.subplots_adjust(left=0.01, right=0.99, top=0.96, bottom=0.06)
        self.heatmap_canvas.draw_idle()

    def _update_overview_colorbar(self, image, label: str) -> None:
        if not hasattr(self, "_heatmap_colorbar") or self._heatmap_colorbar is None:
            self._heatmap_colorbar = self.heatmap_figure.colorbar(image, ax=self.heatmap_axis, pad=0.02)
        else:
            self._heatmap_colorbar.update_normal(image)
        self._heatmap_colorbar.set_label(label, fontsize=8)
        self._heatmap_colorbar.ax.tick_params(labelsize=7, length=0)
        self._heatmap_colorbar.outline.set_visible(False)

    def _remove_overview_colorbar(self) -> None:
        if hasattr(self, "_heatmap_colorbar") and self._heatmap_colorbar is not None:
            self._heatmap_colorbar.remove()
            self._heatmap_colorbar = None

    def _update_stats_panel(self, start_time: float) -> None:
        if not self.recording or self.selected_channel is None:
            self.stats_text.set("Select a channel")
            if self.stats_label is not None:
                self.stats_label.configure(fg="#000000")
            return

        start, end = self._time_window_indices(start_time)
        channel = self.recording.channels[self.selected_channel]
        signal = self._extract_channel_window(channel.index, start, end)
        rms = float(np.sqrt(np.mean(np.square(signal))))
        mean_abs = float(np.mean(np.abs(signal)))
        min_uv = float(signal.min())
        max_uv = float(signal.max())
        ptp = max_uv - min_uv

        window = np.asarray(self.recording.amplifier[start:end, :], dtype=np.float32) * INTAN_UV_PER_BIT
        activity_rank = np.ptp(window, axis=0)
        order = np.argsort(activity_rank)[::-1]
        rank = int(np.where(order == channel.index)[0][0]) + 1
        threshold_text = "n/a"
        event_count_text = "n/a"
        if self.event_thresholds is not None:
            centered = signal - np.median(signal)
            threshold = float(self.event_thresholds[channel.index])
            threshold_crossings = np.sum((centered[1:] < threshold) & (centered[:-1] >= threshold))
            threshold_text = f"{threshold:.1f} uV"
            event_count_text = str(int(threshold_crossings))

        if self.stats_label is not None:
            self.stats_label.configure(fg=matplotlib.colors.to_hex(self._channel_color(self.selected_channel)))

        self.stats_text.set(
            "\n".join(
                [
                    f"{channel.label}",
                    f"Electrode {channel.electrode_id}",
                    f"({channel.x:g}, {channel.y:g})",
                    "",
                    f"Window {start_time:.4f}s",
                    f"to {start_time + self.window_seconds.get():.4f}s",
                    "",
                    f"Min {min_uv:.1f} uV",
                    f"Max {max_uv:.1f} uV",
                    f"P2P {ptp:.1f} uV",
                    f"RMS {rms:.1f} uV",
                    f"Mean |V| {mean_abs:.1f} uV",
                    f"Threshold {threshold_text}",
                    f"Events {event_count_text}",
                    "",
                    f"Activity rank {rank}/{self.recording.channel_count}",
                    f"Display scale x{self.voltage_scale.get():.2f}",
                ]
            )
        )

    def _draw_detail(self, start_time: float) -> None:
        self.detail_axis.clear()
        if not self.recording or self.selected_channel is None:
            self.detail_axis.text(0.5, 0.5, "Select a channel", ha="center", va="center", fontsize=14)
            self.detail_axis.set_axis_off()
            self.detail_canvas.draw_idle()
            return

        zoom_seconds = max(min(self.window_seconds.get() * 0.25, 0.01), 0.002)
        sample_radius = max(int(zoom_seconds * self.recording.sample_rate / 2), 1)
        center_sample = int((start_time + self.window_seconds.get() / 2) * self.recording.sample_rate)
        start = max(center_sample - sample_radius, 0)
        end = min(center_sample + sample_radius, self.recording.sample_count)
        channel = self.recording.channels[self.selected_channel]
        signal = self._extract_channel_window(channel.index, start, end) * max(self.voltage_scale.get(), 1e-6)
        time_axis = np.arange(start, end, dtype=np.float32) / self.recording.sample_rate
        self.detail_axis.plot(time_axis, signal, color=self._channel_color(self.selected_channel), linewidth=1.1)
        self.detail_axis.axvline(center_sample / self.recording.sample_rate, color="#ffffff", linewidth=0.9, alpha=0.8)
        self.detail_axis.set_xticks([])
        self.detail_axis.set_yticks([])
        for spine in self.detail_axis.spines.values():
            spine.set_visible(False)
        self.detail_axis.margins(x=0, y=0.08)
        self.detail_figure.subplots_adjust(left=0.02, right=0.98, top=0.98, bottom=0.05)
        self.detail_canvas.draw_idle()

    def start_heatmap_worker(self) -> None:
        if not self.recording:
            return
        recording = self.recording

        def worker() -> None:
            thresholds = estimate_event_thresholds(recording)
            order = sorted(
                range(len(recording.channels)),
                key=lambda idx: (recording.channels[idx].y, recording.channels[idx].x),
            )
            x_coords = np.array([recording.channels[idx].x for idx in range(recording.channel_count)], dtype=np.float32)
            y_coords = np.array([recording.channels[idx].y for idx in range(recording.channel_count)], dtype=np.float32)
            bin_count = max(180, min(420, int(recording.duration_seconds / 2)))
            edges = np.linspace(0, recording.sample_count, bin_count + 1, dtype=int)
            activity_matrix = np.zeros((len(order), bin_count), dtype=np.float32)
            event_matrix = np.zeros((len(order), bin_count), dtype=np.float32)
            rms_matrix = np.zeros((len(order), bin_count), dtype=np.float32)
            ptp_matrix = np.zeros((len(order), bin_count), dtype=np.float32)
            x_centers = np.zeros(bin_count, dtype=np.float32)
            y_centers = np.zeros(bin_count, dtype=np.float32)
            population_activity = np.zeros(bin_count, dtype=np.float32)
            population_events = np.zeros(bin_count, dtype=np.float32)
            common_mode = np.zeros(bin_count, dtype=np.float32)

            for bin_idx in range(bin_count):
                start = edges[bin_idx]
                end = edges[bin_idx + 1]
                if end <= start:
                    continue
                chunk = np.asarray(recording.amplifier[start:end, :], dtype=np.float32) * INTAN_UV_PER_BIT
                centered = chunk - np.median(chunk, axis=0, keepdims=True)
                activity = np.mean(np.abs(chunk), axis=0)
                rms = np.sqrt(np.mean(centered * centered, axis=0))
                ptp = np.ptp(centered, axis=0)
                crossings = (centered[1:] < thresholds) & (centered[:-1] >= thresholds)
                events = crossings.sum(axis=0).astype(np.float32)

                activity_matrix[:, bin_idx] = activity[order]
                event_matrix[:, bin_idx] = events[order]
                rms_matrix[:, bin_idx] = rms[order]
                ptp_matrix[:, bin_idx] = ptp[order]
                population_activity[bin_idx] = float(activity.mean())
                population_events[bin_idx] = float(events.sum())
                common_mode[bin_idx] = float(np.sqrt(np.mean(np.square(np.mean(centered, axis=1)))))

                baseline = float(np.median(activity))
                weights = np.clip(activity - baseline, a_min=0.0, a_max=None)
                weight_sum = float(weights.sum())
                if weight_sum <= 1e-6:
                    weights = np.clip(activity, a_min=0.0, a_max=None)
                    weight_sum = float(weights.sum())
                if weight_sum > 1e-6:
                    x_centers[bin_idx] = float(np.dot(weights, x_coords) / weight_sum)
                    y_centers[bin_idx] = float(np.dot(weights, y_coords) / weight_sum)

            times = ((edges[:-1] + edges[1:]) / 2.0) / recording.sample_rate
            x_shift = x_centers - np.median(x_centers)
            y_shift = y_centers - np.median(y_centers)
            drift = np.sqrt(x_shift * x_shift + y_shift * y_shift)
            motion = np.vstack([x_shift, y_shift, drift]).astype(np.float32)
            self.heatmap_queue.put(
                (
                    activity_matrix,
                    times,
                    order,
                    motion,
                    event_matrix,
                    rms_matrix,
                    ptp_matrix,
                    population_activity,
                    population_events,
                    common_mode,
                    thresholds,
                )
            )

        threading.Thread(target=worker, daemon=True).start()

    def _poll_heatmap_queue(self) -> None:
        try:
            while True:
                (
                    matrix,
                    times,
                    order,
                    motion,
                    event_matrix,
                    rms_matrix,
                    ptp_matrix,
                    population_activity,
                    population_events,
                    common_mode,
                    thresholds,
                ) = self.heatmap_queue.get_nowait()
                self.heatmap_matrix = matrix
                self.heatmap_times = times
                self.heatmap_order = order
                self.motion_matrix = motion
                self.event_matrix = event_matrix
                self.rms_matrix = rms_matrix
                self.ptp_matrix = ptp_matrix
                self.population_activity = population_activity
                self.population_events = population_events
                self.common_mode_series = common_mode
                self.event_thresholds = thresholds
                self.status_text.set("Overview ready. Click it to jump through the recording.")
                self._draw_heatmap()
        except queue.Empty:
            pass
        self.root.after(200, self._poll_heatmap_queue)

    def on_heatmap_click(self, event) -> None:
        if event.inaxes != self.heatmap_axis or not self.recording or event.xdata is None:
            return
        target = min(max(float(event.xdata), 0.0), max(self.recording.duration_seconds - self.window_seconds.get(), 0.0))
        self.time_seconds.set(target)
        self.time_slider.set(target)
        self.refresh_views()


def main() -> None:
    root = tk.Tk()
    app = SpikeViewerApp(root)
    default_info = Path.cwd() / "info.rhd"
    if default_info.exists():
        app.load_info(default_info)
    root.mainloop()


if __name__ == "__main__":
    main()
