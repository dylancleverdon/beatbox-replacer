Beatbox Replacer
================

Turns beatboxing into MIDI drum notes in Ableton Live.

The plug-in is installed for all users at:

    C:\Program Files\Common Files\VST3\BeatboxReplacer.vst3

This folder only holds this readme and the uninstaller.


Setting it up in Ableton Live (10.1 or later)
---------------------------------------------

1. Open Live and rescan plug-ins (Options > Settings/Preferences > Plug-Ins: make sure
   "Use VST3 Plug-In System Folders" is on, then click Rescan).
2. Track 1: an audio track with your beatbox (recorded or live from your mic).
3. Track 2: a MIDI track with BeatboxReplacer. In the device, open the sidechain section,
   enable Sidechain and set Audio From to the beatbox track.
4. Track 3: a MIDI track with a Drum Rack. Set MIDI From to Track 2, the lower chooser to
   "BeatboxReplacer", Monitor to In, and arm it to record the notes.

Drums are told apart by note number (Live merges MIDI channels on internal routing).
Default notes: kick C1 (36), rim C#1 (37), snare D1 (38), clap D#1 (39),
closed hat F#1 (42), open hat A#1 (46).

You can also use Capture in the plug-in and drag the resulting MIDI clip into Live.


Updates
-------

The plug-in checks for new versions when its window opens and can install them itself.
Restart Live afterwards. You can always download the latest installer from:

    https://github.com/dylancleverdon/beatbox-replacer/releases/latest


Uninstalling
------------

Use Settings > Apps (or Control Panel > Programs) > Beatbox Replacer > Uninstall.
