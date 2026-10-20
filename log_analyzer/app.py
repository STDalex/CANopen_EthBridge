#!/usr/bin/env python3
"""Russian-language desktop viewer for CanSniff .CSL logs."""

from __future__ import annotations

import argparse
import csv
import os
import queue
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

APP_DIR = Path(__file__).resolve().parent
PROJECT_DIR = APP_DIR.parent
if str(PROJECT_DIR) not in sys.path:
    sys.path.insert(0, str(PROJECT_DIR))

from log_analyzer.analysis import (  # noqa: E402
    AnalysisResult,
    SessionInfo,
    analyze_session,
    available_message_types,
    available_nodes,
    canopen_decode,
    close_reason_text,
    describe_frame,
    filter_frames,
    frame_id_text,
    integrity_text,
    normalize_logs_root,
    scan_sessions,
)


DEFAULT_CARD_PATH = r"G:\CANSNIFF"
MAX_VISIBLE_ROWS = 50_000


def human_size(size: int) -> str:
    value = float(size)
    for unit in ("Б", "КБ", "МБ", "ГБ"):
        if value < 1024 or unit == "ГБ":
            return f"{value:.1f} {unit}" if unit != "Б" else f"{int(value)} Б"
        value /= 1024
    return f"{value:.1f} ГБ"


def format_duration(microseconds: int) -> str:
    seconds = microseconds / 1_000_000
    if seconds < 60:
        return f"{seconds:.3f} с"
    minutes, seconds = divmod(seconds, 60)
    return f"{int(minutes)} мин {seconds:.1f} с"


class CanSniffAnalyzer(tk.Tk):
    def __init__(self, initial_path: str) -> None:
        super().__init__()
        self.title("CanSniff — анализатор CAN-логов")
        self.geometry("1440x880")
        self.minsize(1050, 650)
        self.option_add("*Font", ("Segoe UI", 10))

        self.sessions: list[SessionInfo] = []
        self.session_by_iid: dict[str, SessionInfo] = {}
        self.result: AnalysisResult | None = None
        self.filtered_frames = []
        self.worker_queue: queue.Queue = queue.Queue()
        self.busy = False
        self.path_var = tk.StringVar(value=initial_path)
        self.status_var = tk.StringVar(value="Готово")
        self.summary_var = tk.StringVar(value="Выберите папку с логами")
        self.search_var = tk.StringVar()
        self.type_var = tk.StringVar(value="Все")
        self.node_var = tk.StringVar(value="Все")
        self.changed_var = tk.BooleanVar(value=False)
        self.sort_labels: dict[int, dict[str, str]] = {}

        self._configure_style()
        self._build_ui()
        self.after(100, self._poll_worker)
        self.after(250, self.scan)

    def _configure_style(self) -> None:
        style = ttk.Style(self)
        available = style.theme_names()
        if "vista" in available:
            style.theme_use("vista")
        style.configure("Treeview", rowheight=25)
        style.configure("Treeview.Heading", font=("Segoe UI Semibold", 10))
        style.configure("Title.TLabel", font=("Segoe UI Semibold", 13))
        style.configure("Summary.TLabel", font=("Segoe UI Semibold", 11))

    def _build_ui(self) -> None:
        toolbar = ttk.Frame(self, padding=(10, 10, 10, 6))
        toolbar.pack(fill=tk.X)
        ttk.Label(toolbar, text="Папка карты / LOGS:").pack(side=tk.LEFT)
        ttk.Entry(toolbar, textvariable=self.path_var).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=8)
        ttk.Button(toolbar, text="Обзор…", command=self.browse).pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(toolbar, text="Обновить", command=self.scan).pack(side=tk.LEFT)

        paned = ttk.Panedwindow(self, orient=tk.HORIZONTAL)
        paned.pack(fill=tk.BOTH, expand=True, padx=10)

        left = ttk.Frame(paned, padding=(0, 0, 8, 0))
        right = ttk.Frame(paned)
        paned.add(left, weight=1)
        paned.add(right, weight=4)

        ttk.Label(left, text="Сессии", style="Title.TLabel").pack(anchor=tk.W, pady=(0, 6))
        session_columns = ("session", "bitrate", "mode", "files", "size", "state")
        self.session_tree = ttk.Treeview(left, columns=session_columns, show="headings", selectmode="browse")
        session_labels = {
            "session": "Сессия",
            "bitrate": "кбит/с",
            "mode": "Режим",
            "files": "Файлы",
            "size": "Размер",
            "state": "Состояние",
        }
        widths = {"session": 85, "bitrate": 68, "mode": 75, "files": 55, "size": 80, "state": 115}
        for column in session_columns:
            self.session_tree.heading(column, text=session_labels[column])
            self.session_tree.column(column, width=widths[column], minwidth=45, anchor=tk.CENTER)
        self._enable_sorting(self.session_tree, session_labels)
        session_scroll = ttk.Scrollbar(left, command=self.session_tree.yview)
        self.session_tree.configure(yscrollcommand=session_scroll.set)
        self.session_tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        session_scroll.pack(side=tk.LEFT, fill=tk.Y)
        self.session_tree.bind("<<TreeviewSelect>>", self._on_session_selected)

        top = ttk.Frame(right)
        top.pack(fill=tk.X)
        ttk.Label(top, textvariable=self.summary_var, style="Summary.TLabel").pack(side=tk.LEFT, anchor=tk.W)
        ttk.Button(top, text="Открыть папку", command=self.open_session_folder).pack(side=tk.RIGHT)

        self.notebook = ttk.Notebook(right)
        self.notebook.pack(fill=tk.BOTH, expand=True, pady=(8, 0))
        self._build_overview_tab()
        self._build_frames_tab()
        self._build_ids_tab()
        self._build_files_tab()

        status = ttk.Frame(self, padding=(10, 5, 10, 8))
        status.pack(fill=tk.X)
        self.progress = ttk.Progressbar(status, mode="indeterminate", length=180)
        self.progress.pack(side=tk.RIGHT)
        ttk.Label(status, textvariable=self.status_var).pack(side=tk.LEFT)

    def _build_overview_tab(self) -> None:
        tab = ttk.Frame(self.notebook, padding=12)
        self.notebook.add(tab, text="Обзор")
        self.overview_text = tk.Text(tab, wrap=tk.WORD, relief=tk.FLAT, padx=8, pady=8)
        self.overview_text.pack(fill=tk.BOTH, expand=True)
        self.overview_text.configure(state=tk.DISABLED)

    def _build_frames_tab(self) -> None:
        tab = ttk.Frame(self.notebook, padding=8)
        self.notebook.add(tab, text="Кадры")
        filters = ttk.Frame(tab)
        filters.pack(fill=tk.X, pady=(0, 7))
        ttk.Label(filters, text="Поиск ID/данных/описания:").pack(side=tk.LEFT)
        search = ttk.Entry(filters, textvariable=self.search_var, width=24)
        search.pack(side=tk.LEFT, padx=(5, 10))
        search.bind("<Return>", lambda _event: self.apply_filters())
        ttk.Label(filters, text="Тип:").pack(side=tk.LEFT)
        self.type_combo = ttk.Combobox(filters, textvariable=self.type_var, state="readonly", width=17)
        self.type_combo["values"] = ("Все",)
        self.type_combo.pack(side=tk.LEFT, padx=(5, 10))
        ttk.Label(filters, text="Узел:").pack(side=tk.LEFT)
        self.node_combo = ttk.Combobox(filters, textvariable=self.node_var, state="readonly", width=8)
        self.node_combo["values"] = ("Все",)
        self.node_combo.pack(side=tk.LEFT, padx=(5, 10))
        ttk.Checkbutton(filters, text="Только изменения", variable=self.changed_var).pack(side=tk.LEFT)
        ttk.Button(filters, text="Применить", command=self.apply_filters).pack(side=tk.LEFT, padx=8)
        ttk.Button(filters, text="Сбросить", command=self.reset_filters).pack(side=tk.LEFT)
        ttk.Button(filters, text="Экспорт CSV…", command=self.export_csv).pack(side=tk.RIGHT)

        columns = ("time", "delta", "id", "type", "node", "canopen", "dlc", "data", "description")
        self.frame_tree = ttk.Treeview(tab, columns=columns, show="headings")
        labels = {
            "time": "Время, с",
            "delta": "Δ, мс",
            "id": "CAN ID",
            "type": "Кадр",
            "node": "Узел",
            "canopen": "CANopen",
            "dlc": "DLC",
            "data": "Данные",
            "description": "Расшифровка",
        }
        widths = {"time": 105, "delta": 85, "id": 105, "type": 55, "node": 55, "canopen": 120, "dlc": 45, "data": 210, "description": 380}
        for column in columns:
            self.frame_tree.heading(column, text=labels[column])
            self.frame_tree.column(column, width=widths[column], minwidth=40, anchor=tk.W)
        self._enable_sorting(self.frame_tree, labels)
        yscroll = ttk.Scrollbar(tab, orient=tk.VERTICAL, command=self.frame_tree.yview)
        xscroll = ttk.Scrollbar(tab, orient=tk.HORIZONTAL, command=self.frame_tree.xview)
        self.frame_tree.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)
        self.frame_tree.pack(fill=tk.BOTH, expand=True, side=tk.TOP)
        xscroll.pack(fill=tk.X, side=tk.BOTTOM)
        yscroll.place(relx=1, rely=0.05, relheight=0.9, anchor=tk.NE)

    def _build_ids_tab(self) -> None:
        tab = ttk.Frame(self.notebook, padding=8)
        self.notebook.add(tab, text="CAN ID и частоты")
        columns = ("id", "type", "node", "canopen", "count", "percent", "rate", "avg", "min", "max", "dlc", "changes", "last")
        self.id_tree = ttk.Treeview(tab, columns=columns, show="headings")
        labels = {
            "id": "CAN ID", "type": "Кадр", "node": "Узел", "canopen": "CANopen",
            "count": "Кадров", "percent": "%", "rate": "Гц", "avg": "Средн. период, мс",
            "min": "Мин., мс", "max": "Макс., мс", "dlc": "DLC", "changes": "Изменений", "last": "Последние данные",
        }
        widths = {"id": 100, "type": 60, "node": 55, "canopen": 120, "count": 85, "percent": 65, "rate": 80, "avg": 125, "min": 85, "max": 85, "dlc": 65, "changes": 90, "last": 220}
        for column in columns:
            self.id_tree.heading(column, text=labels[column])
            self.id_tree.column(column, width=widths[column], minwidth=45, anchor=tk.W)
        self._enable_sorting(self.id_tree, labels)
        yscroll = ttk.Scrollbar(tab, orient=tk.VERTICAL, command=self.id_tree.yview)
        xscroll = ttk.Scrollbar(tab, orient=tk.HORIZONTAL, command=self.id_tree.xview)
        self.id_tree.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)
        self.id_tree.pack(fill=tk.BOTH, expand=True)
        xscroll.pack(fill=tk.X)
        yscroll.place(relx=1, rely=0, relheight=0.94, anchor=tk.NE)

    def _build_files_tab(self) -> None:
        tab = ttk.Frame(self.notebook, padding=8)
        self.notebook.add(tab, text="Целостность файлов")
        columns = ("file", "blocks", "frames", "close")
        self.file_tree = ttk.Treeview(tab, columns=columns, show="headings")
        file_labels = {}
        for column, label, width in (
            ("file", "Файл", 240), ("blocks", "Блоков", 100),
            ("frames", "Кадров", 120), ("close", "Завершение", 260),
        ):
            file_labels[column] = label
            self.file_tree.heading(column, text=label)
            self.file_tree.column(column, width=width, anchor=tk.W)
        self._enable_sorting(self.file_tree, file_labels)
        self.file_tree.pack(fill=tk.BOTH, expand=True)

    def browse(self) -> None:
        initial = self.path_var.get()
        if not Path(initial).exists():
            initial = str(Path.home())
        selected = filedialog.askdirectory(title="Выберите CANSNIFF или LOGS", initialdir=initial)
        if selected:
            self.path_var.set(selected)
            self.scan()

    def _set_busy(self, busy: bool, status: str) -> None:
        self.busy = busy
        self.status_var.set(status)
        if busy:
            self.progress.start(12)
        else:
            self.progress.stop()

    def _run_worker(self, kind: str, function, *args) -> None:
        if self.busy:
            return
        self._set_busy(True, "Чтение данных…")

        def target() -> None:
            try:
                value = function(*args)
                self.worker_queue.put((kind, value, None))
            except Exception as error:  # GUI boundary: present all failures without crashing.
                self.worker_queue.put((kind, None, error))

        threading.Thread(target=target, daemon=True).start()

    def _poll_worker(self) -> None:
        try:
            kind, value, error = self.worker_queue.get_nowait()
        except queue.Empty:
            self.after(100, self._poll_worker)
            return
        self._set_busy(False, "Готово")
        if error is not None:
            messagebox.showerror("Ошибка", str(error), parent=self)
            self.status_var.set(f"Ошибка: {error}")
        elif kind == "scan":
            self._show_sessions(value)
        elif kind == "analysis":
            self._show_analysis(value)
        self.after(100, self._poll_worker)

    def scan(self) -> None:
        self._run_worker("scan", scan_sessions, self.path_var.get().strip())

    def _show_sessions(self, sessions: list[SessionInfo]) -> None:
        self.sessions = sessions
        self.session_by_iid.clear()
        self.session_tree.delete(*self.session_tree.get_children())
        for index, session in enumerate(sessions):
            iid = f"s{index}"
            self.session_by_iid[iid] = session
            self.session_tree.insert(
                "", tk.END, iid=iid,
                values=(session.name, session.bitrate // 1000 if session.bitrate else "—", session.mode,
                        session.file_count, human_size(session.total_bytes), session.state),
            )
        self.status_var.set(f"Найдено сессий: {len(sessions)}")
        self.summary_var.set(f"Сессий: {len(sessions)} • выберите запись слева")
        if sessions:
            last_iid = f"s{len(sessions) - 1}"
            self.session_tree.selection_set(last_iid)
            self.session_tree.focus(last_iid)
            self.session_tree.see(last_iid)
            self._load_selected_session()

    def _on_session_selected(self, _event=None) -> None:
        self.after_idle(self._load_selected_session)

    def _load_selected_session(self) -> None:
        selection = self.session_tree.selection()
        if not selection or self.busy:
            return
        session = self.session_by_iid.get(selection[0])
        if session is not None:
            self.summary_var.set(f"{session.name}: проверка CRC и анализ…")
            self._run_worker("analysis", analyze_session, session)

    def _show_analysis(self, result: AnalysisResult) -> None:
        self.result = result
        load = "—" if result.estimated_bus_load_percent is None else f"{result.estimated_bus_load_percent:.1f}%"
        self.summary_var.set(
            f"{result.session.name} • {len(result.frames):,} кадров • {len(result.id_statistics)} CAN ID • "
            f"{format_duration(result.duration_us)} • нагрузка ≈ {load} • {integrity_text(result)}".replace(",", " ")
        )
        self._fill_overview(result)
        self._fill_ids(result)
        self._fill_files(result)
        self.type_combo["values"] = ("Все", *available_message_types(result.frames))
        self.node_combo["values"] = ("Все", *(str(node) for node in available_nodes(result.frames)))
        self.reset_filters()
        self.status_var.set(f"Прочитано кадров: {len(result.frames):,}".replace(",", " "))

    def _fill_overview(self, result: AnalysisResult) -> None:
        session = result.session
        frame_types = {}
        nodes = set()
        for frame in result.frames:
            node, message_type = canopen_decode(frame)
            if node is not None:
                nodes.add(node)
            if message_type:
                frame_types[message_type] = frame_types.get(message_type, 0) + 1
        lines = [
            f"Сессия: {session.name}",
            f"Путь: {session.path}",
            f"Прошивка: {session.firmware or 'не указана'}",
            f"Скорость CAN: {session.bitrate / 1000:g} кбит/с" if session.bitrate else "Скорость CAN: не определена",
            f"Режим: {session.mode}",
            f"Выбранные узлы: {', '.join(map(str, session.selected_nodes)) or 'все'}",
            "",
            f"Кадров: {len(result.frames):,}".replace(",", " "),
            f"Уникальных CAN ID: {len(result.id_statistics)}",
            f"Длительность: {format_duration(result.duration_us)}",
            f"Обнаруженные CANopen-узлы: {', '.join(map(str, sorted(nodes))) or 'нет'}",
            f"Оценочная нагрузка шины: {result.estimated_bus_load_percent:.2f}%" if result.estimated_bus_load_percent is not None else "Оценочная нагрузка шины: —",
            f"Целостность: {integrity_text(result)}",
        ]
        if frame_types:
            lines.extend(("", "CANopen-сообщения:"))
            lines.extend(f"  {name}: {count:,}".replace(",", " ") for name, count in sorted(frame_types.items(), key=lambda item: -item[1]))
        if result.warnings:
            lines.extend(("", "Предупреждения:"))
            lines.extend(f"  • {warning}" for warning in result.warnings)
        if result.errors:
            lines.extend(("", "Ошибки:"))
            lines.extend(f"  • {error}" for error in result.errors)
        if session.metadata_error:
            lines.extend(("", f"Метаданные: {session.metadata_error}"))
        self.overview_text.configure(state=tk.NORMAL)
        self.overview_text.delete("1.0", tk.END)
        self.overview_text.insert("1.0", "\n".join(lines))
        self.overview_text.configure(state=tk.DISABLED)

    def _fill_ids(self, result: AnalysisResult) -> None:
        self.id_tree.delete(*self.id_tree.get_children())
        for item in result.id_statistics:
            can_id = f"0x{item.can_id:08X}" if item.extended else f"0x{item.can_id:03X}"
            fmt = lambda value: "—" if value is None else f"{value:.3f}"
            self.id_tree.insert("", tk.END, values=(
                can_id, item.frame_type, item.node_id if item.node_id is not None else "—",
                item.canopen_type or "—", item.count, f"{item.percent:.2f}", f"{item.rate_hz:.2f}",
                fmt(item.average_period_ms), fmt(item.min_period_ms), fmt(item.max_period_ms),
                item.dlcs, item.changes, item.last_data,
            ))

    def _fill_files(self, result: AnalysisResult) -> None:
        self.file_tree.delete(*self.file_tree.get_children())
        for summary in result.file_summaries:
            self.file_tree.insert("", tk.END, values=(summary.path.name, summary.blocks, summary.frames, close_reason_text(summary)))
        for error in result.errors:
            self.file_tree.insert("", tk.END, values=(error.split(":", 1)[0], "—", "—", error))

    def reset_filters(self) -> None:
        self.search_var.set("")
        self.type_var.set("Все")
        self.node_var.set("Все")
        self.changed_var.set(False)
        self.apply_filters()

    def apply_filters(self) -> None:
        if self.result is None:
            return
        node_text = self.node_var.get()
        node = int(node_text) if node_text.isdigit() else None
        self.filtered_frames = list(filter_frames(
            self.result.frames,
            query=self.search_var.get(),
            message_type=self.type_var.get(),
            node_id=node,
            data_changed_only=self.changed_var.get(),
        ))
        self.frame_tree.delete(*self.frame_tree.get_children())
        previous_timestamp = None
        for frame in self.filtered_frames[:MAX_VISIBLE_ROWS]:
            node_id, message_type = canopen_decode(frame)
            delta = "—" if previous_timestamp is None else f"{(frame.timestamp_us - previous_timestamp) / 1000:.3f}"
            previous_timestamp = frame.timestamp_us
            self.frame_tree.insert("", tk.END, values=(
                f"{frame.timestamp_us / 1_000_000:.6f}", delta, frame_id_text(frame),
                "RTR" if frame.remote else "DATA", node_id if node_id is not None else "—",
                message_type or "—", frame.dlc, frame.data.hex(" ").upper(), describe_frame(frame),
            ))
        suffix = f"; показаны первые {MAX_VISIBLE_ROWS:,}".replace(",", " ") if len(self.filtered_frames) > MAX_VISIBLE_ROWS else ""
        self.status_var.set(f"После фильтра: {len(self.filtered_frames):,} кадров{suffix}".replace(",", " "))

    def export_csv(self) -> None:
        if not self.filtered_frames:
            messagebox.showinfo("Экспорт", "Нет кадров для экспорта", parent=self)
            return
        session_name = self.result.session.name if self.result else "capture"
        path = filedialog.asksaveasfilename(
            title="Экспортировать отфильтрованные кадры",
            defaultextension=".csv",
            initialfile=f"{session_name}_filtered.csv",
            filetypes=(("CSV", "*.csv"), ("Все файлы", "*.*")),
        )
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8-sig", newline="") as output:
                writer = csv.writer(output, delimiter=";")
                writer.writerow(("timestamp_us", "timestamp_s", "can_id", "frame_type", "dlc", "data_hex", "node_id", "canopen_type", "description"))
                for frame in self.filtered_frames:
                    node_id, message_type = canopen_decode(frame)
                    writer.writerow((frame.timestamp_us, f"{frame.timestamp_us / 1_000_000:.6f}", frame_id_text(frame),
                                     "RTR" if frame.remote else "DATA", frame.dlc, frame.data.hex().upper(),
                                     "" if node_id is None else node_id, message_type, describe_frame(frame)))
            self.status_var.set(f"Экспортировано: {path}")
        except OSError as error:
            messagebox.showerror("Ошибка экспорта", str(error), parent=self)

    def open_session_folder(self) -> None:
        if self.result is None:
            return
        path = str(self.result.session.path)
        try:
            if os.name == "nt":
                os.startfile(path)  # type: ignore[attr-defined]
            elif sys.platform == "darwin":
                subprocess.Popen(["open", path])
            else:
                subprocess.Popen(["xdg-open", path])
        except OSError as error:
            messagebox.showerror("Ошибка", str(error), parent=self)

    def _enable_sorting(self, tree: ttk.Treeview, labels: dict[str, str]) -> None:
        """Make every data heading clickable and remember its plain caption."""
        self.sort_labels[id(tree)] = dict(labels)
        for column in tree["columns"]:
            tree.heading(
                column,
                command=lambda current=column, target=tree: self._sort_tree(
                    target, current, False
                ),
            )

    @staticmethod
    def _parse_sort_value(column: str, raw: str):
        """Parse displayed values so numbers, sizes and hexadecimal IDs sort naturally."""
        text = raw.strip()
        if text in ("", "—"):
            return None
        upper = text.upper()
        if column == "id" and upper.startswith("0X"):
            try:
                return int(upper, 16)
            except ValueError:
                pass
        if column == "session" and upper.startswith("S") and upper[1:].isdigit():
            return int(upper[1:])
        if column == "size":
            parts = upper.replace(",", ".").split()
            multipliers = {"Б": 1, "КБ": 1024, "МБ": 1024**2, "ГБ": 1024**3}
            if len(parts) == 2 and parts[1] in multipliers:
                try:
                    return float(parts[0]) * multipliers[parts[1]]
                except ValueError:
                    pass
        numeric_columns = {
            "bitrate", "files", "time", "delta", "node", "dlc", "count",
            "percent", "rate", "avg", "min", "max", "changes", "blocks", "frames",
        }
        if column in numeric_columns:
            try:
                return float(text.replace(" ", "").replace(",", ".").rstrip("%"))
            except ValueError:
                pass
        return text.casefold()

    def _sort_tree(self, tree: ttk.Treeview, column: str, reverse: bool) -> None:
        values = []
        missing = []
        for position, item in enumerate(tree.get_children("")):
            value = self._parse_sort_value(column, tree.set(item, column))
            if value is None:
                missing.append((position, item))
            else:
                values.append((value, position, item))
        values.sort(key=lambda entry: (entry[0], entry[1]), reverse=reverse)
        ordered = [item for _value, _position, item in values]
        ordered.extend(item for _position, item in missing)
        for position, item in enumerate(ordered):
            tree.move(item, "", position)

        labels = self.sort_labels.get(id(tree), {})
        for current in tree["columns"]:
            base = labels.get(current, str(current))
            tree.heading(current, text=base)
        arrow = "▼" if reverse else "▲"
        tree.heading(
            column,
            text=f"{labels.get(column, column)} {arrow}",
            command=lambda: self._sort_tree(tree, column, not reverse),
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Графический анализатор логов CanSniff")
    parser.add_argument("path", nargs="?", default=DEFAULT_CARD_PATH, help="CANSNIFF, LOGS или папка сессии")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    app = CanSniffAnalyzer(args.path)
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

