import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';

const WATCH_COMMAND = ['open-pods', '--waybar-watch'];
const RESTART_DELAY_SECONDS = 5;

// Width of a battery meter track, in px. The fill width is derived from this.
const TRACK_WIDTH = 132;

function spawn(argv) {
    try {
        GLib.spawn_async(
            null,
            argv,
            null,
            GLib.SpawnFlags.SEARCH_PATH,
            null
        );
    } catch (error) {
        logError(error, `open-pods: failed to run ${argv.join(' ')}`);
    }
}

function openTui() {
    const terminal = GLib.find_program_in_path('gnome-terminal');
    if (terminal !== null) {
        spawn(['gnome-terminal', '--', 'open-pods']);
        return;
    }

    const console = GLib.find_program_in_path('kgx');
    if (console !== null) {
        spawn(['kgx', '--', 'open-pods']);
        return;
    }

    spawn(['x-terminal-emulator', '-e', 'open-pods']);
}

// Turn the multi-line tooltip into structured battery entries.
// Lines look like "L: 80%", "R: 75%", "C: 90%" or a bare "80%" for headphones.
function parseBatteries(tooltip) {
    const entries = [];
    const lines = (tooltip ?? '').split('\n').slice(1);

    for (const raw of lines) {
        const line = raw.trim();
        if (line === '')
            continue;

        let match = line.match(/^([LRC])\s*:\s*(\d+)\s*%$/i);
        if (match !== null) {
            const key = match[1].toUpperCase();
            const label = key === 'L' ? 'Left' : key === 'R' ? 'Right' : 'Case';
            entries.push({label, value: parseInt(match[2], 10)});
            continue;
        }

        match = line.match(/^(\d+)\s*%$/);
        if (match !== null)
            entries.push({label: 'Battery', value: parseInt(match[1], 10)});
    }

    return entries;
}

const OpenPodsIndicator = GObject.registerClass(
class OpenPodsIndicator extends PanelMenu.Button {
    _init() {
        super._init(0.0, 'open-pods');

        this._destroyed = false;
        this._process = null;
        this._stdout = null;
        this._cancellable = null;
        this._restartSource = 0;
        this._lastStatus = {
            text: '',
            tooltip: 'No AirPods',
            class: 'disconnected',
            percentage: 0,
        };

        this._buildPanel();
        this._buildMenu();
        this._startWatcher();
    }

    destroy() {
        this._destroyed = true;
        this._stopWatcher();
        super.destroy();
    }

    _buildPanel() {
        this._panelBox = new St.BoxLayout({
            style_class: 'open-pods-panel-label',
            y_align: Clutter.ActorAlign.CENTER,
        });

        this._panelIcon = new St.Icon({
            icon_name: 'audio-headphones-symbolic',
            style_class: 'open-pods-panel-icon',
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._panelBox.add_child(this._panelIcon);

        this._label = new St.Label({
            text: '--',
            y_align: Clutter.ActorAlign.CENTER,
            style_class: 'open-pods-panel-text',
        });
        this._panelBox.add_child(this._label);

        this.add_child(this._panelBox);
    }

    _buildMenu() {
        this.menu.removeAll();
        this.menu.box.add_style_class_name('open-pods-glass');

        const card = new St.BoxLayout({
            vertical: true,
            style_class: 'open-pods-card',
        });

        // Header: icon + device name + connection state.
        const header = new St.BoxLayout({style_class: 'open-pods-header'});

        this._headerIcon = new St.Icon({
            icon_name: 'audio-headphones-symbolic',
            style_class: 'open-pods-header-icon',
            y_align: Clutter.ActorAlign.CENTER,
        });
        header.add_child(this._headerIcon);

        const headerText = new St.BoxLayout({
            vertical: true,
            y_align: Clutter.ActorAlign.CENTER,
            style_class: 'open-pods-header-text',
        });
        this._headerName = new St.Label({
            text: 'AirPods',
            style_class: 'open-pods-header-name',
        });
        this._headerStatus = new St.Label({
            text: 'Not connected',
            style_class: 'open-pods-header-status',
        });
        headerText.add_child(this._headerName);
        headerText.add_child(this._headerStatus);
        header.add_child(headerText);

        card.add_child(header);

        // Battery meters (rebuilt on each status update).
        this._batteryBox = new St.BoxLayout({
            vertical: true,
            style_class: 'open-pods-batteries',
        });
        card.add_child(this._batteryBox);

        this._emptyLabel = new St.Label({
            text: 'No AirPods nearby',
            style_class: 'open-pods-empty',
        });
        card.add_child(this._emptyLabel);

        // Action pills.
        const actions = new St.BoxLayout({style_class: 'open-pods-actions'});

        const openBtn = new St.Button({
            label: 'Open',
            style_class: 'open-pods-btn',
            x_expand: true,
            can_focus: true,
        });
        openBtn.connect('clicked', () => {
            this.menu.close();
            openTui();
        });
        actions.add_child(openBtn);

        const reclaimBtn = new St.Button({
            label: 'Reclaim Audio',
            style_class: 'open-pods-btn',
            x_expand: true,
            can_focus: true,
        });
        reclaimBtn.connect('clicked', () => {
            this.menu.close();
            spawn(['open-pods', '--reclaim']);
        });
        actions.add_child(reclaimBtn);

        card.add_child(actions);

        const item = new PopupMenu.PopupBaseMenuItem({
            reactive: false,
            can_focus: false,
            style_class: 'open-pods-card-item',
        });
        item.add_child(card);
        this.menu.addMenuItem(item);
    }

    _rebuildBatteries(entries) {
        this._batteryBox.destroy_all_children();

        for (const entry of entries) {
            const row = new St.BoxLayout({style_class: 'open-pods-batt-row'});

            const name = new St.Label({
                text: entry.label,
                style_class: 'open-pods-batt-name',
                y_align: Clutter.ActorAlign.CENTER,
            });
            row.add_child(name);

            const track = new St.BoxLayout({
                style_class: 'open-pods-track',
                y_align: Clutter.ActorAlign.CENTER,
            });
            const clamped = Math.max(0, Math.min(100, entry.value));
            const fill = new St.Widget({style_class: 'open-pods-fill'});
            fill.set_style(`width: ${Math.round(TRACK_WIDTH * clamped / 100)}px;`);
            if (clamped <= 20)
                fill.add_style_class_name('low');
            track.add_child(fill);
            row.add_child(track);

            const pct = new St.Label({
                text: `${clamped}%`,
                style_class: 'open-pods-batt-pct',
                y_align: Clutter.ActorAlign.CENTER,
            });
            row.add_child(pct);

            this._batteryBox.add_child(row);
        }
    }

    _startWatcher() {
        this._stopWatcher();
        this._cancellable = new Gio.Cancellable();

        try {
            this._process = Gio.Subprocess.new(
                WATCH_COMMAND,
                Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_SILENCE
            );
            this._stdout = new Gio.DataInputStream({
                base_stream: this._process.get_stdout_pipe(),
            });
            this._readNextLine();
        } catch (error) {
            logError(error, 'open-pods: failed to start status watcher');
            this._scheduleRestart();
        }
    }

    _stopWatcher() {
        if (this._restartSource !== 0) {
            GLib.Source.remove(this._restartSource);
            this._restartSource = 0;
        }

        if (this._cancellable !== null) {
            this._cancellable.cancel();
            this._cancellable = null;
        }

        if (this._process !== null) {
            this._process.force_exit();
            this._process = null;
        }

        this._stdout = null;
    }

    _scheduleRestart() {
        if (this._destroyed || this._restartSource !== 0)
            return;

        this._restartSource = GLib.timeout_add_seconds(
            GLib.PRIORITY_DEFAULT,
            RESTART_DELAY_SECONDS,
            () => {
                this._restartSource = 0;
                this._startWatcher();
                return GLib.SOURCE_REMOVE;
            }
        );
    }

    _readNextLine() {
        if (this._destroyed || this._stdout === null || this._cancellable === null)
            return;

        this._stdout.read_line_async(
            GLib.PRIORITY_DEFAULT,
            this._cancellable,
            (stream, result) => {
                if (this._destroyed)
                    return;

                try {
                    const [line] = stream.read_line_finish_utf8(result);
                    if (line === null) {
                        this._scheduleRestart();
                        return;
                    }

                    this._handleStatusLine(line);
                    this._readNextLine();
                } catch (error) {
                    if (!this._cancellable?.is_cancelled()) {
                        logError(error, 'open-pods: failed to read status line');
                        this._scheduleRestart();
                    }
                }
            }
        );
    }

    _handleStatusLine(line) {
        let status;
        try {
            status = JSON.parse(line);
        } catch (error) {
            logError(error, `open-pods: invalid status JSON: ${line}`);
            return;
        }

        this._lastStatus = {
            text: status.text ?? '',
            tooltip: status.tooltip ?? 'No AirPods',
            class: status.class ?? 'unknown',
            percentage: status.percentage ?? 0,
        };
        this._updateUi();
    }

    _updateUi() {
        const connected = this._lastStatus.class !== 'disconnected';
        const text = this._lastStatus.text;
        const name = this._lastStatus.tooltip.split('\n')[0] || 'AirPods';

        // Panel: compact icon + percentage.
        this._label.text = connected && text !== '' ? text : '--';
        if (connected)
            this._panelBox.remove_style_class_name('disconnected');
        else
            this._panelBox.add_style_class_name('disconnected');

        // Menu header.
        this._headerName.text = connected ? name : 'AirPods';
        this._headerStatus.text = connected ? 'Connected' : 'Not connected';

        const entries = connected ? parseBatteries(this._lastStatus.tooltip) : [];
        this._rebuildBatteries(entries);

        const hasEntries = entries.length > 0;
        this._batteryBox.visible = hasEntries;
        this._emptyLabel.visible = !hasEntries;
    }
});

export default class OpenPodsExtension extends Extension {
    enable() {
        this._indicator = new OpenPodsIndicator();
        Main.panel.addToStatusArea(this.uuid, this._indicator);
    }

    disable() {
        this._indicator?.destroy();
        this._indicator = null;
    }
}
