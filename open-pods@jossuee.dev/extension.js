import Cairo from 'gi://cairo';
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Pango from 'gi://Pango';
import St from 'gi://St';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';

const WATCH_COMMAND = ['open-pods', '--waybar-watch'];
const RESTART_DELAY_SECONDS = 5;

// Noise-control segments. `id` matches the open-pods --set-noise argument and
// the `noise` field emitted by the daemon. `cap` gates visibility by model.
const NOISE_SEGMENTS = [
    {id: 'off', label: 'Off', cap: null},
    {id: 'transparency', label: 'Transparency', cap: 'anc'},
    {id: 'adaptive', label: 'Adaptive', cap: 'adaptive'},
    {id: 'anc', label: 'ANC', cap: 'anc'},
];

const NOISE_NAMES = {
    off: 'Off',
    transparency: 'Transparency',
    adaptive: 'Adaptive',
    anc: 'Noise Cancellation',
};

function spawn(argv) {
    try {
        GLib.spawn_async(null, argv, null, GLib.SpawnFlags.SEARCH_PATH, null);
    } catch (error) {
        logError(error, `open-pods: failed to run ${argv.join(' ')}`);
    }
}

function openTui() {
    if (GLib.find_program_in_path('gnome-terminal') !== null) {
        spawn(['gnome-terminal', '--', 'open-pods']);
        return;
    }
    if (GLib.find_program_in_path('kgx') !== null) {
        spawn(['kgx', '--', 'open-pods']);
        return;
    }
    spawn(['x-terminal-emulator', '-e', 'open-pods']);
}

// Fallback parser for the legacy tooltip ("L: 80%" lines) when the daemon
// predates the structured `batteries` field.
function parseTooltipBatteries(tooltip) {
    const entries = [];
    for (const raw of (tooltip ?? '').split('\n').slice(1)) {
        const line = raw.trim();
        let m = line.match(/^([LRC])\s*:\s*(\d+)\s*%$/i);
        if (m !== null) {
            const key = m[1].toUpperCase();
            entries.push({
                component: key === 'L' ? 'left' : key === 'R' ? 'right' : 'case',
                level: parseInt(m[2], 10),
                charging: false,
            });
            continue;
        }
        m = line.match(/^(\d+)\s*%$/);
        if (m !== null)
            entries.push({component: 'headphone', level: parseInt(m[1], 10), charging: false});
    }
    return entries;
}

const COMPONENT_LABELS = {
    left: 'Left',
    right: 'Right',
    case: 'Case',
    headphone: 'Headphones',
};

// ─────────────────────────── MPRIS media bridge ───────────────────────────
// Talks to org.mpris.MediaPlayer2.* over the session bus. No backend coupling.
class MediaBridge {
    constructor(onChange) {
        this._onChange = onChange;
        this._bus = Gio.DBus.session;
        this._player = null;
        this.title = '';
        this.artist = '';
        this.artUrl = '';
        this.status = 'Stopped';

        this._propsSignalId = this._bus.signal_subscribe(
            null,
            'org.freedesktop.DBus.Properties',
            'PropertiesChanged',
            '/org/mpris/MediaPlayer2',
            null,
            Gio.DBusSignalFlags.NONE,
            () => this.refresh()
        );
        this._nameSignalId = this._bus.signal_subscribe(
            'org.freedesktop.DBus',
            'org.freedesktop.DBus',
            'NameOwnerChanged',
            '/org/freedesktop/DBus',
            null,
            Gio.DBusSignalFlags.NONE,
            (conn, sender, path, iface, signal, params) => {
                const [name] = params.deepUnpack();
                if (typeof name === 'string' && name.startsWith('org.mpris.MediaPlayer2.'))
                    this.refresh();
            }
        );
    }

    destroy() {
        if (this._propsSignalId) {
            this._bus.signal_unsubscribe(this._propsSignalId);
            this._propsSignalId = 0;
        }
        if (this._nameSignalId) {
            this._bus.signal_unsubscribe(this._nameSignalId);
            this._nameSignalId = 0;
        }
        this._onChange = null;
    }

    _call(dest, path, iface, method, params, cb) {
        this._bus.call(
            dest, path, iface, method, params, null,
            Gio.DBusCallFlags.NONE, 1000, null,
            (conn, res) => {
                try {
                    cb(conn.call_finish(res));
                } catch (_e) {
                    cb(null);
                }
            }
        );
    }

    _getProp(player, prop, cb) {
        this._call(
            player, '/org/mpris/MediaPlayer2',
            'org.freedesktop.DBus.Properties', 'Get',
            new GLib.Variant('(ss)', ['org.mpris.MediaPlayer2.Player', prop]),
            (result) => {
                if (result === null) {
                    cb(null);
                    return;
                }
                try {
                    cb(result.recursiveUnpack()[0]);
                } catch (_e) {
                    cb(null);
                }
            }
        );
    }

    refresh() {
        this._call(
            'org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus', 'ListNames', null,
            (result) => {
                let names = [];
                if (result !== null) {
                    try {
                        names = result.recursiveUnpack()[0]
                            .filter(n => n.startsWith('org.mpris.MediaPlayer2.'));
                    } catch (_e) {}
                }
                if (names.length === 0) {
                    this._player = null;
                    this.title = '';
                    this.artist = '';
                    this.artUrl = '';
                    this.status = 'Stopped';
                    this._emit();
                    return;
                }
                this._choosePlaying(names, 0, names[0]);
            }
        );
    }

    // Prefer the first player reporting "Playing"; otherwise the first one.
    _choosePlaying(names, index, fallback) {
        if (index >= names.length) {
            this._loadFrom(fallback);
            return;
        }
        this._getProp(names[index], 'PlaybackStatus', (status) => {
            if (status === 'Playing') {
                this._loadFrom(names[index]);
                return;
            }
            this._choosePlaying(names, index + 1, fallback);
        });
    }

    _loadFrom(player) {
        this._player = player;
        this._getProp(player, 'PlaybackStatus', (status) => {
            this.status = typeof status === 'string' ? status : 'Stopped';
            this._getProp(player, 'Metadata', (meta) => {
                let title = '';
                let artist = '';
                let artUrl = '';
                if (meta && typeof meta === 'object') {
                    title = meta['xesam:title'] ?? '';
                    const a = meta['xesam:artist'];
                    artist = Array.isArray(a) ? a.join(', ') : (a ?? '');
                    artUrl = meta['mpris:artUrl'] ?? '';
                }
                this.title = title;
                this.artist = artist;
                this.artUrl = typeof artUrl === 'string' ? artUrl : '';
                this._emit();
            });
        });
    }

    _emit() {
        if (this._onChange)
            this._onChange();
    }

    get isPlaying() {
        return this.status === 'Playing';
    }

    get hasPlayer() {
        return this._player !== null;
    }

    _control(method) {
        if (this._player === null) {
            this.refresh();
            return;
        }
        this._call(
            this._player, '/org/mpris/MediaPlayer2',
            'org.mpris.MediaPlayer2.Player', method, null,
            () => this.refresh()
        );
    }

    playPause() { this._control('PlayPause'); }
    next() { this._control('Next'); }
    previous() { this._control('Previous'); }
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
        this._view = 'main';
        this._status = {
            connected: false,
            name: 'AirPods',
            text: '',
            noise: '',
            caps: {anc: false, adaptive: false},
            batteries: [],
        };

        this._marqueeTimeout = 0;
        this._marqueeLabel = null;
        this._marqueeWidth = 0;

        this._media = new MediaBridge(() => this._updateMedia());

        this._buildPanel();
        this._buildMenu();

        this.menu.connect('open-state-changed', (_menu, open) => {
            if (open) {
                this._media.refresh();
            } else {
                this._stopMarquee();
            }
        });

        this._startWatcher();
    }

    destroy() {
        this._destroyed = true;
        this._stopMarquee();
        this._stopWatcher();
        this._media?.destroy();
        this._media = null;
        super.destroy();
    }

    // ───────────────────────────── Panel ─────────────────────────────
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

    // ───────────────────────────── Menu ─────────────────────────────
    _buildMenu() {
        this.menu.removeAll();
        this.menu.box.add_style_class_name('open-pods-glass');

        const root = new St.BoxLayout({vertical: true, style_class: 'open-pods-card'});

        this._buildHeader(root);

        // The two stacked views share the same slot; only one is visible.
        this._mainView = new St.BoxLayout({vertical: true, style_class: 'open-pods-view'});
        this._settingsView = new St.BoxLayout({vertical: true, style_class: 'open-pods-view'});
        this._settingsView.hide();

        this._buildMainView(this._mainView);
        this._buildSettingsView(this._settingsView);

        root.add_child(this._mainView);
        root.add_child(this._settingsView);

        const item = new PopupMenu.PopupBaseMenuItem({
            reactive: false,
            can_focus: false,
            style_class: 'open-pods-card-item',
        });
        item.add_child(root);
        this.menu.addMenuItem(item);

        this._updateUi();
    }

    _buildHeader(root) {
        const header = new St.BoxLayout({style_class: 'open-pods-header'});

        const text = new St.BoxLayout({
            vertical: true,
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
            style_class: 'open-pods-header-text',
        });
        this._headerName = new St.Label({text: 'AirPods', style_class: 'open-pods-header-name'});
        this._headerStatus = new St.Label({
            text: 'Not connected',
            style_class: 'open-pods-header-status',
        });
        text.add_child(this._headerName);
        text.add_child(this._headerStatus);
        header.add_child(text);

        // Overall-battery ring: AirPods glyph inside, percentage beneath.
        this._headerRingLevel = 0;
        this._headerRing = new St.BoxLayout({
            vertical: true,
            style_class: 'open-pods-header-ring-wrap',
            y_align: Clutter.ActorAlign.CENTER,
        });
        const ringBox = new St.Widget({
            layout_manager: new Clutter.BinLayout(),
            style_class: 'open-pods-header-ring-box',
        });
        this._headerRingArea = new St.DrawingArea({style_class: 'open-pods-header-ring'});
        this._headerRingArea.connect('repaint', () => this._drawHeaderRing());
        const ringIcon = new St.Icon({
            icon_name: 'audio-headphones-symbolic',
            style_class: 'open-pods-header-ring-icon',
            x_align: Clutter.ActorAlign.CENTER,
            y_align: Clutter.ActorAlign.CENTER,
            x_expand: true,
            y_expand: true,
        });
        ringBox.add_child(this._headerRingArea);
        ringBox.add_child(ringIcon);
        this._headerRing.add_child(ringBox);
        this._headerRing.hide();
        header.add_child(this._headerRing);

        this._gearButton = new St.Button({
            style_class: 'open-pods-icon-btn',
            child: new St.Icon({icon_name: 'emblem-system-symbolic', icon_size: 16}),
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._gearButton.connect('clicked', () => this._switchView());
        header.add_child(this._gearButton);

        root.add_child(header);
    }

    _drawHeaderRing() {
        const area = this._headerRingArea;
        const cr = area.get_context();
        const [w, h] = area.get_surface_size();
        const lineWidth = 3.5;
        const radius = Math.min(w, h) / 2 - lineWidth - 1;
        const cx = w / 2;
        const cy = h / 2;
        const level = Math.max(0, Math.min(100, this._headerRingLevel));

        cr.setLineCap(Cairo.LineCap.ROUND);

        // Soft outer glow — a faint, wider stroke under the track.
        cr.setLineWidth(lineWidth + 3);
        cr.setSourceRGBA(1, 1, 1, 0.05);
        cr.arc(cx, cy, radius, 0, 2 * Math.PI);
        cr.stroke();

        // Track.
        cr.setLineWidth(lineWidth);
        cr.setSourceRGBA(1, 1, 1, 0.14);
        cr.arc(cx, cy, radius, 0, 2 * Math.PI);
        cr.stroke();

        if (level > 0) {
            const start = -Math.PI / 2;
            const end = start + 2 * Math.PI * (level / 100);

            // Progress arc (grayscale; dims when low).
            cr.setLineWidth(lineWidth);
            cr.setSourceRGBA(1, 1, 1, level <= 20 ? 0.55 : 0.92);
            cr.arc(cx, cy, radius, start, end);
            cr.stroke();

            // Subtle bright highlight over the first part of the arc.
            const highlightEnd = Math.min(end, start + Math.PI * 0.5);
            cr.setLineWidth(lineWidth - 1.6);
            cr.setSourceRGBA(1, 1, 1, 0.5);
            cr.arc(cx, cy, radius, start, highlightEnd);
            cr.stroke();
        }
        cr.$dispose();
    }

    // ── Main view: Now Playing (hero) + noise control ──
    _buildMainView(view) {
        this._buildMedia(view);
        this._buildNoise(view);
    }

    _buildMedia(view) {
        const card = new St.BoxLayout({vertical: true, style_class: 'open-pods-media'});

        const top = new St.BoxLayout({style_class: 'open-pods-media-top'});

        // Artwork: placeholder underneath, cover art layered on top so a failed
        // remote load gracefully reveals the placeholder.
        this._artBox = new St.Widget({
            layout_manager: new Clutter.BinLayout(),
            style_class: 'open-pods-art',
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._artPlaceholder = new St.Icon({
            icon_name: 'audio-x-generic-symbolic',
            style_class: 'open-pods-art-placeholder',
            x_align: Clutter.ActorAlign.CENTER,
            y_align: Clutter.ActorAlign.CENTER,
            x_expand: true,
            y_expand: true,
        });
        this._artImage = new St.Icon({
            style_class: 'open-pods-art-image',
            x_expand: true,
            y_expand: true,
        });
        this._artImage.hide();
        this._artBox.add_child(this._artPlaceholder);
        this._artBox.add_child(this._artImage);
        top.add_child(this._artBox);

        const meta = new St.BoxLayout({
            vertical: true,
            style_class: 'open-pods-media-meta',
            y_align: Clutter.ActorAlign.CENTER,
        });

        // Title lives in a fixed-width clip box and marquees when it overflows.
        this._titleClip = new St.Widget({
            style_class: 'open-pods-media-title-clip',
            layout_manager: new Clutter.FixedLayout(),
            clip_to_allocation: true,
        });
        this._mediaTitle = new St.Label({
            text: 'Nothing playing',
            style_class: 'open-pods-media-title',
        });
        this._mediaTitle.clutter_text.set_line_wrap(false);
        this._mediaTitle.clutter_text.set_ellipsize(Pango.EllipsizeMode.NONE);
        this._mediaTitle.set_position(0, 0);
        this._titleClip.add_child(this._mediaTitle);
        this._marqueeLabel = this._mediaTitle;
        meta.add_child(this._titleClip);

        this._mediaArtist = new St.Label({text: '', style_class: 'open-pods-media-artist'});
        this._mediaArtist.clutter_text.set_line_wrap(false);
        this._mediaArtist.clutter_text.set_ellipsize(Pango.EllipsizeMode.END);
        meta.add_child(this._mediaArtist);

        top.add_child(meta);
        card.add_child(top);

        const controls = new St.BoxLayout({style_class: 'open-pods-media-controls'});
        const spacerL = new St.Widget({x_expand: true});
        const spacerR = new St.Widget({x_expand: true});

        const prevBtn = this._makeIconButton('media-skip-backward-symbolic', 'open-pods-media-btn',
            () => this._media.previous());
        this._playBtn = this._makeIconButton('media-playback-start-symbolic',
            'open-pods-media-btn open-pods-media-play', () => this._media.playPause());
        const nextBtn = this._makeIconButton('media-skip-forward-symbolic', 'open-pods-media-btn',
            () => this._media.next());

        controls.add_child(spacerL);
        controls.add_child(prevBtn);
        controls.add_child(this._playBtn);
        controls.add_child(nextBtn);
        controls.add_child(spacerR);
        card.add_child(controls);

        view.add_child(card);
    }

    _buildNoise(view) {
        const section = new St.BoxLayout({vertical: true, style_class: 'open-pods-noise'});

        this._segmentBar = new St.BoxLayout({style_class: 'open-pods-segments'});
        this._segments = {};
        for (const seg of NOISE_SEGMENTS) {
            const btn = new St.Button({
                label: seg.label,
                style_class: 'open-pods-segment',
                x_expand: true,
                can_focus: true,
            });
            btn.connect('clicked', () => this._setNoise(seg.id));
            this._segments[seg.id] = btn;
            this._segmentBar.add_child(btn);
        }
        section.add_child(this._segmentBar);

        this._noiseLabel = new St.Label({
            text: '',
            style_class: 'open-pods-noise-label',
            x_align: Clutter.ActorAlign.CENTER,
        });
        section.add_child(this._noiseLabel);

        view.add_child(section);
    }

    // ── Settings view: advanced / less-frequent actions ──
    _buildSettingsView(view) {
        const back = new St.Button({style_class: 'open-pods-back', x_expand: true});
        const backRow = new St.BoxLayout({style_class: 'open-pods-back-row'});
        backRow.add_child(new St.Icon({icon_name: 'go-previous-symbolic', icon_size: 14}));
        backRow.add_child(new St.Label({text: 'Settings', y_align: Clutter.ActorAlign.CENTER}));
        back.set_child(backRow);
        back.connect('clicked', () => this._switchView());
        view.add_child(back);

        this._detailBox = new St.BoxLayout({vertical: true, style_class: 'open-pods-detail'});
        view.add_child(this._detailBox);

        const actions = new St.BoxLayout({vertical: true, style_class: 'open-pods-actions'});
        actions.add_child(this._makeRowButton('audio-speakers-symbolic', 'Reclaim audio',
            'Pull playback back to this device', () => {
                this.menu.close();
                spawn(['open-pods', '--reclaim']);
            }));
        actions.add_child(this._makeRowButton('utilities-terminal-symbolic', 'Open full app',
            'Launch the open-pods terminal UI', () => {
                this.menu.close();
                openTui();
            }));
        view.add_child(actions);
    }

    _makeIconButton(iconName, styleClass, onClick) {
        const btn = new St.Button({
            style_class: styleClass,
            child: new St.Icon({icon_name: iconName, icon_size: 18}),
            can_focus: true,
        });
        btn.connect('clicked', onClick);
        return btn;
    }

    _makeRowButton(iconName, title, subtitle, onClick) {
        const btn = new St.Button({style_class: 'open-pods-row-btn', x_expand: true, can_focus: true});
        const row = new St.BoxLayout({style_class: 'open-pods-row-inner'});
        row.add_child(new St.Icon({
            icon_name: iconName,
            icon_size: 18,
            y_align: Clutter.ActorAlign.CENTER,
            style_class: 'open-pods-row-icon',
        }));
        const text = new St.BoxLayout({vertical: true, x_expand: true, y_align: Clutter.ActorAlign.CENTER});
        text.add_child(new St.Label({text: title, style_class: 'open-pods-row-title'}));
        text.add_child(new St.Label({text: subtitle, style_class: 'open-pods-row-sub'}));
        row.add_child(text);
        btn.set_child(row);
        btn.connect('clicked', onClick);
        return btn;
    }

    _switchView() {
        const showSettings = this._view === 'main';
        const from = showSettings ? this._mainView : this._settingsView;
        const to = showSettings ? this._settingsView : this._mainView;
        this._view = showSettings ? 'settings' : 'main';

        from.ease({
            opacity: 0,
            duration: 120,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            onComplete: () => {
                from.hide();
                to.opacity = 0;
                to.show();
                to.ease({
                    opacity: 255,
                    duration: 150,
                    mode: Clutter.AnimationMode.EASE_OUT_QUAD,
                });
            },
        });
    }

    // ─────────────────────────── Rendering ───────────────────────────
    _rebuildDetails(entries) {
        this._detailBox.destroy_all_children();
        const title = new St.Label({text: 'Battery', style_class: 'open-pods-detail-title'});
        this._detailBox.add_child(title);
        if (entries.length === 0) {
            this._detailBox.add_child(new St.Label({
                text: 'No data available',
                style_class: 'open-pods-detail-sub',
            }));
            return;
        }
        const rings = new St.BoxLayout({
            style_class: 'open-pods-detail-rings',
            x_align: Clutter.ActorAlign.CENTER,
            x_expand: true,
        });
        for (const entry of entries)
            rings.add_child(this._makeBatteryRing(entry));
        this._detailBox.add_child(rings);
    }

    // Apple-style ring: track + progress arc (Cairo), percentage centred,
    // component label beneath. Used in the Settings battery detail.
    _makeBatteryRing(entry) {
        const clamped = Math.max(0, Math.min(100, entry.level));

        const wrap = new St.BoxLayout({vertical: true, style_class: 'open-pods-ring-wrap'});

        const overlay = new St.Widget({layout_manager: new Clutter.BinLayout()});
        const area = new St.DrawingArea({style_class: 'open-pods-ring'});
        area.connect('repaint', () => {
            const cr = area.get_context();
            const [w, h] = area.get_surface_size();
            const lineWidth = 3.5;
            const radius = Math.min(w, h) / 2 - lineWidth - 1;
            const cx = w / 2;
            const cy = h / 2;

            cr.setLineCap(Cairo.LineCap.ROUND);

            cr.setLineWidth(lineWidth + 3);
            cr.setSourceRGBA(1, 1, 1, 0.05);
            cr.arc(cx, cy, radius, 0, 2 * Math.PI);
            cr.stroke();

            cr.setLineWidth(lineWidth);
            cr.setSourceRGBA(1, 1, 1, 0.14);
            cr.arc(cx, cy, radius, 0, 2 * Math.PI);
            cr.stroke();

            if (clamped > 0) {
                const start = -Math.PI / 2;
                const end = start + 2 * Math.PI * (clamped / 100);
                cr.setLineWidth(lineWidth);
                cr.setSourceRGBA(1, 1, 1, clamped <= 20 ? 0.55 : 0.92);
                cr.arc(cx, cy, radius, start, end);
                cr.stroke();
            }
            cr.$dispose();
        });
        overlay.add_child(area);
        overlay.add_child(new St.Label({
            text: `${clamped}`,
            style_class: 'open-pods-ring-pct',
            x_align: Clutter.ActorAlign.CENTER,
            y_align: Clutter.ActorAlign.CENTER,
            x_expand: true,
            y_expand: true,
        }));
        wrap.add_child(overlay);

        const labelRow = new St.BoxLayout({
            style_class: 'open-pods-ring-label-row',
            x_align: Clutter.ActorAlign.CENTER,
        });
        if (entry.charging) {
            labelRow.add_child(new St.Icon({
                icon_name: 'battery-good-charging-symbolic',
                style_class: 'open-pods-ring-bolt',
                y_align: Clutter.ActorAlign.CENTER,
            }));
        }
        labelRow.add_child(new St.Label({
            text: COMPONENT_LABELS[entry.component] ?? entry.component,
            style_class: 'open-pods-ring-label',
            y_align: Clutter.ActorAlign.CENTER,
        }));
        wrap.add_child(labelRow);

        return wrap;
    }

    _updateMedia() {
        if (!this._mediaTitle)
            return;
        const playing = this._media.hasPlayer && this._media.title !== '';

        this._setTitle(playing ? this._media.title : 'Nothing playing');
        // Keep the artist line allocated even when empty so the row never jumps.
        this._mediaArtist.text = playing && this._media.artist !== '' ? this._media.artist : ' ';

        this._setArtwork(playing ? this._media.artUrl : '');

        const icon = this._playBtn.get_child();
        icon.icon_name = this._media.isPlaying
            ? 'media-playback-pause-symbolic'
            : 'media-playback-start-symbolic';
    }

    _setArtwork(artUrl) {
        if (!this._artImage)
            return;
        if (artUrl && artUrl.length > 0) {
            try {
                const file = Gio.File.new_for_uri(artUrl);
                this._artImage.gicon = new Gio.FileIcon({file});
                this._artImage.show();
                return;
            } catch (_e) {
                // fall through to placeholder
            }
        }
        this._artImage.gicon = null;
        this._artImage.hide();
    }

    // ─────────────────────────── Marquee ───────────────────────────
    _setTitle(text) {
        this._stopMarquee();
        this._mediaTitle.text = text;
        if (!this.menu.isOpen)
            return;
        // Measure once layout settles, then scroll only if it overflows.
        GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
            if (this._destroyed || !this.menu.isOpen)
                return GLib.SOURCE_REMOVE;
            this._startMarquee();
            return GLib.SOURCE_REMOVE;
        });
    }

    _stopMarquee() {
        if (this._marqueeTimeout !== 0) {
            GLib.Source.remove(this._marqueeTimeout);
            this._marqueeTimeout = 0;
        }
        if (this._marqueeLabel !== null) {
            this._marqueeLabel.remove_all_transitions();
            this._marqueeLabel.translation_x = 0;
        }
    }

    _startMarquee() {
        const label = this._marqueeLabel;
        if (label === null)
            return;
        const boxWidth = this._titleClip.get_width();
        const [, natWidth] = label.clutter_text.get_preferred_width(-1);
        if (natWidth <= boxWidth || boxWidth <= 0) {
            label.translation_x = 0;
            return;
        }

        const distance = Math.ceil(natWidth - boxWidth + 8);
        const duration = Math.max(1200, Math.round(distance / 30 * 1000));

        const scrollBack = () => {
            if (this._destroyed)
                return;
            label.ease({
                translation_x: 0,
                duration,
                mode: Clutter.AnimationMode.LINEAR,
                onComplete: () => {
                    if (this._destroyed)
                        return;
                    this._marqueeTimeout = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1400, () => {
                        this._marqueeTimeout = 0;
                        scrollOut();
                        return GLib.SOURCE_REMOVE;
                    });
                },
            });
        };
        const scrollOut = () => {
            if (this._destroyed)
                return;
            label.ease({
                translation_x: -distance,
                duration,
                mode: Clutter.AnimationMode.LINEAR,
                onComplete: () => {
                    if (this._destroyed)
                        return;
                    this._marqueeTimeout = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1400, () => {
                        this._marqueeTimeout = 0;
                        scrollBack();
                        return GLib.SOURCE_REMOVE;
                    });
                },
            });
        };

        this._marqueeTimeout = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1000, () => {
            this._marqueeTimeout = 0;
            scrollOut();
            return GLib.SOURCE_REMOVE;
        });
    }

    _updateNoise() {
        const caps = this._status.caps;
        for (const seg of NOISE_SEGMENTS) {
            const btn = this._segments[seg.id];
            const supported = seg.cap === null || caps[seg.cap];
            btn.visible = this._status.connected && supported;
            if (seg.id === this._status.noise)
                btn.add_style_class_name('active');
            else
                btn.remove_style_class_name('active');
        }
        const known = this._status.connected && NOISE_NAMES[this._status.noise];
        this._segmentBar.visible = this._status.connected;
        this._noiseLabel.visible = !!known;
        this._noiseLabel.text = known ? NOISE_NAMES[this._status.noise] : '';
    }

    _setNoise(id) {
        // Optimistic highlight; the watcher confirms the real device state.
        this._status.noise = id;
        this._updateNoise();
        spawn(['open-pods', '--set-noise', id]);
    }

    _updateUi() {
        const s = this._status;

        this._label.text = s.connected && s.text !== '' ? s.text : '--';
        if (s.connected)
            this._panelBox.remove_style_class_name('disconnected');
        else
            this._panelBox.add_style_class_name('disconnected');

        this._headerName.text = s.connected ? s.name : 'AirPods';
        this._headerStatus.text = s.connected ? 'Connected' : 'Not connected';

        const entries = s.connected ? s.batteries : [];
        this._rebuildDetails(entries);

        // Overall battery: daemon percentage, else the lowest reported cell.
        let overall = s.percentage;
        if (!overall && entries.length > 0)
            overall = Math.min(...entries.map((e) => e.level));
        const showRing = s.connected && entries.length > 0;
        this._headerRing.visible = showRing;
        if (showRing) {
            this._headerRingLevel = overall;
            this._headerRingArea.queue_repaint();
        }

        this._updateNoise();
        this._updateMedia();
    }

    // ─────────────────────────── Watcher ───────────────────────────
    _startWatcher() {
        this._stopWatcher();
        this._cancellable = new Gio.Cancellable();
        try {
            this._process = Gio.Subprocess.new(
                WATCH_COMMAND,
                Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_SILENCE
            );
            this._stdout = new Gio.DataInputStream({base_stream: this._process.get_stdout_pipe()});
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
        let raw;
        try {
            raw = JSON.parse(line);
        } catch (error) {
            logError(error, `open-pods: invalid status JSON: ${line}`);
            return;
        }

        const connected = (raw.class ?? 'disconnected') !== 'disconnected';
        const tooltip = raw.tooltip ?? '';
        let batteries = Array.isArray(raw.batteries) ? raw.batteries : null;
        if (batteries === null)
            batteries = parseTooltipBatteries(tooltip);

        this._status = {
            connected,
            name: raw.name ?? (tooltip.split('\n')[0] || 'AirPods'),
            text: raw.text ?? '',
            percentage: raw.percentage ?? 0,
            noise: raw.noise ?? '',
            caps: {
                anc: raw.caps?.anc ?? true,
                adaptive: raw.caps?.adaptive ?? false,
            },
            batteries: connected ? batteries : [],
        };
        this._updateUi();
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
