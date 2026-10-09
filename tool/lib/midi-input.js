//
// Web MIDI input: enumerates devices, forwards messages with their
// timestamps, optional single-device filter (the DAW's record source).
//
//   const midi = await setupMidi({ onMessage: (data, timeStamp, input) => ..., statusEl });
//   midi.inputs()            -> [{id, name}]
//   midi.select(id | null)   -> only this device (null = all)
//
export async function setupMidi({ onMessage, statusEl = null, onDevicesChanged = null } = {}) {
  const setStatus = (t) => { if (statusEl) statusEl.textContent = t; };
  const ctl = {
    access: null,
    selected: null,
    inputs() {
      if (!this.access) return [];
      return [...this.access.inputs.values()].map((i) => ({ id: i.id, name: i.name }));
    },
    select(id) { this.selected = id || null; },
    destroy() {
      if (!this.access) return;
      for (const input of this.access.inputs.values()) input.onmidimessage = null;
      this.access.onstatechange = null;
    },
  };
  if (!navigator.requestMIDIAccess) {
    setStatus('MIDI: unsupported');
    return ctl;
  }
  try {
    ctl.access = await navigator.requestMIDIAccess();
  } catch {
    setStatus('MIDI: permission denied');
    return ctl;
  }
  const attach = () => {
    const names = [];
    for (const input of ctl.access.inputs.values()) {
      names.push(input.name);
      input.onmidimessage = (e) => {
        if (ctl.selected && input.id !== ctl.selected) return;
        onMessage?.(e.data, e.timeStamp, input);
      };
    }
    setStatus(names.length ? 'MIDI: ' + names.join(', ') : 'MIDI: no devices');
    onDevicesChanged?.(ctl.inputs());
  };
  ctl.access.onstatechange = attach;
  attach();
  return ctl;
}
