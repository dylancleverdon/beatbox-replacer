# Beatbox Replacer – user guide

Beatbox Replacer listens to your beatboxing and turns every hit into a MIDI note. You teach it
what *your* kick, snare and hi-hat sound like, and you can add up to 8 sounds. It works in
Ableton Live 11 and 12 on Windows and Mac (on Mac, use the VST3 version).

## 1. Set up Ableton (one time per Set)

Ableton can't record the MIDI a plug-in makes on the same track, so the setup uses three tracks:

| Track | Type | What's on it |
|---|---|---|
| **Beatbox** | Audio | Your beatbox recording, or your mic (Monitor **In** or **Auto** if you beatbox live) |
| **Replacer** | MIDI | **Beatbox Replacer** |
| **Drums** | MIDI | A **Drum Rack** (or any drum instrument) |

1. On the **Replacer** track, open Beatbox Replacer's device view. Expand the **Sidechain**
   section in the device's title bar, switch it on, and set **Audio From** to the **Beatbox**
   track. Pre FX or Post FX both work.
2. On the **Drums** track set **MIDI From** to the **Replacer** track, and in the chooser below it
   pick **BeatboxReplacer**. Set **Monitor** to **In**.
3. Play the Beatbox track. The top of the plug-in should say **Hearing your beatbox**. If it says
   *No sidechain signal*, check step 1.

Sounds are told apart by note number, not MIDI channel. Live merges channels when it routes MIDI
between tracks.

## 2. Teach it your sounds (Learn tab)

1. Press **Learn**. It waits for you to press play in Ableton.
2. Play a section where you do each of your sounds **5–10 times or more**. Small gaps between
   hits help. 8–16 bars is plenty.
3. Stop playback. Beatbox Replacer finds every hit and puts similar hits into **groups**.
4. For each group, press ▶ to hear a typical hit, then pick which sound it is from the dropdown.
   Pick **Ignore (no note)** for breaths, clicks or anything that shouldn't make a note.
5. If one hit landed in the wrong group, click its marker on the waveform to hear it (it plays up
   to the next marker), then right-click it (or click it again) to move just that hit.
6. Fix the markers if you need to. To delete one, right-click it and choose **Delete marker**, or
   select it and press Delete. To add a missed hit, right-click an empty spot on the waveform and
   choose **Add marker here**. The new marker joins the group it sounds most like.
7. Too many or too few groups? Use **Groups − / +**.
8. Press **Play recording** to hear the whole pass (press it again to stop). To start partway,
   right-click the waveform and choose **Play from here**.
9. Want this pass as MIDI? Drag **Drag this as MIDI into Ableton** (bottom left) onto a MIDI track.
   Every hit with a sound becomes a note, and ignored hits are left out. A pass recorded with
   Learn goes back to the bar it was recorded at. A loaded file starts at bar 1. Do this before
   step 10, because adding to training clears the recording.
10. Press **Add to training**. Use **Replace training** instead to start over.

You can repeat Learn as often as you like. Every pass adds more examples, and more examples make
it more accurate. You can also drop an audio file of your beatboxing on the Learn tab, or use
**Load audio file…**.

**Your sounds** (right side): rename a sound, change its note, delete it, or **+ Add sound**
(open hat, clap, rim, toms…). Notes use Ableton's names: C1 = 36 (kick), D1 = 38 (snare),
F#1 = 42 (closed hat), A#1 = 46 (open hat). These match a Drum Rack's default pads.

Your training is saved inside your Ableton Set automatically.

## 3. Get MIDI out (Play tab)

### Way A – live

With **Live MIDI** on, every hit plays a note on the Drums track as it happens. Arm the
**Drums** track and record, either while the Beatbox track plays or while you beatbox into the
mic. The pads on the Play tab flash for each hit, so you can see what it's hearing.

Each note arrives about one **Listen window** (20 ms by default) after the hit starts, because
the plug-in has to hear that much of the sound before it can tell which one it is. Quantize the
recorded clip, nudge it earlier, or use Way B for exact timing.

### Way B – capture and drag (exact timing)

1. Turn on **Capture**. It says *Armed – press play in Ableton*.
2. Play the beatbox section, then stop. The clip appears in the piano roll.
3. Drag **Drag MIDI clip into Ableton** onto a MIDI track at the bar it tells you, e.g. *drop at
   bar 5*. With **Clip starts at: Song start**, drop it at bar 1 instead.
4. If dragging doesn't work, use **Save .mid…** and drag the file in from Explorer or Finder.

Notes in a captured clip sit exactly where each hit started, with no delay. If you re-train
afterwards, the last clip is re-classified with the new training.

**Convert audio file…** turns a beatbox recording into a clip directly, without playing it.

## 4. Detection settings (Play tab)

| Setting | Default | What it does | Change it when |
|---|---|---|---|
| Sensitivity gate | −48 dB | A hit must be louder than this | Quiet hats are missed → lower it. Background noise triggers → raise it |
| Attack rise | 9 dB | How sharply the level must jump to count as a new hit | Double triggers → raise it. Soft hits missed → lower it |
| Min gap | 60 ms | Shortest time between two hits | One sound makes two notes → raise it. Fast rolls are missed → lower it |
| Listen window | 20 ms | How much of each hit is analysed. This is also the live delay | Sounds get mixed up → try 30–40 ms. Want less live delay → try 12–15 ms. Changing it re-analyses your training |
| Velocity follows loudness | on | Louder hits give higher velocities | Turn off for every note at the same velocity |
| Fixed velocity | 100 | Velocity when the above is off | |
| Note length | 60 ms | Length of each note | Drum Racks ignore it, so it rarely matters |

## 5. Profiles (Setup tab)

- **Export…** saves your sounds and training to a file. **Import…** loads one.
- **Save as default** makes new instances of the plug-in start with your current sounds and
  training.

## 6. Updates

The plug-in checks for a new version whenever you open its window, and **Check now** checks
again. When one is available, click **Install update**, then restart Ableton. If it says it
couldn't update in place, click **Run installer**. That downloads the normal installer and opens
it.

## 7. Troubleshooting

- **"No sidechain signal"**: the device's Sidechain must be on with Audio From set to your
  beatbox track, and that track must be playing or monitoring your mic.
- **Plug-in not in Ableton's browser**: Preferences → Plug-ins → turn on *Use VST3 Plug-in
  System Folders* → Rescan. On Mac, look for the VST3 version.
- **Hits light up but the Drums track gets no MIDI**: check *MIDI From* (Replacer track, then
  BeatboxReplacer below it) and set Monitor to **In**.
- **Wrong sounds**: Learn again with more examples of the sounds it confuses. Mark breaths and
  clicks as **Ignore**. Try a longer Listen window.
- **Mac says it can't open the installer**: System Settings → Privacy & Security → Open Anyway.
- **Windows SmartScreen warning**: More info → Run anyway. The installer isn't code-signed.

## 8. How is this different from Ableton's "Convert Drums to New MIDI Track"?

Live's converter guesses kick, snare and hi-hat from fixed rules, with no way to teach it, and
only works on finished audio clips. Beatbox Replacer learns *your* sounds from examples you
label, supports up to 8 sounds on any notes, ignores sounds you mark as noise, and also works
live while you perform.
