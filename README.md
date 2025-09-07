# ondes_framboise

HISTORY

The Ondes framboise is a digital emulator of one of the first electronic musical instruments, the Ondes Martenot. Invented by Maurice Martenot in 1928, the Ondes Martenot uses interference between radio frequency oscillators, one fixed and the other variable, to generate an audio signal.

In its most mature form the Ondes Martenot has both a conventional small-keyed 6-octave keyboard which can be moved side to side to generate vibrato and also a ribbon - a ring attached to a thread which slides from side to side to create glissandi or portamento effects. The input method is selected by a switch.

The volume is controlled by a 'Touche' (a large button which increases the volume the harder it is pressed) or alternatively by an expression foot pedal.

There are multiple waveforms - Ondes (sine), Creux (peak-limited triangle), Gambe (square), Nasillard (pulse) - which are switch selectable singly or in combination, plus 3 other waveforms which can be mixed into the sound at variable levels - Octaviant (full-wave rectified sine), petit Gambe (square) and Souffle (pink noise). The Tutti switch enables all waveforms except Souffle at the same time, overiding the individual selections.

Perhaps the most distinctive features of the instrument are the multiple loudspeakers or 'diffuseurs' which can also be selected singly or in combination. These are the Principal (an ordinary speaker), the Résonance (a loudspeaker mechanically and acoustically coupled to resonating springs), the Métallique (a metal gong fitted with an audio transducer) and, perhaps most characteristic of all, the Palme (a distinctively shaped resonator box fitted with 2 chromatic octaves of resonating strings whose bridge is driven by an audio transducer). The latter two give extremely distinctive colouration to the sound output. The level of the 'special effect' diffuseurs can be adjusted relative to the Principal.

Other features include a variable low-pass filter (Feutre) controlled by a foot pedal, transposition buttons which allow the pitch to be changed briefly (± a quarter tone, + a semitone, + a whole tone, + a major third, + a perfect 5th, ± an octave), and (if my understanding is correct!) a selector for the normal legato mode (where the sound generation is controlled only by the Touche, regardless of whether a key is pressed or not) and claquement (click) mode, where sound will only be generated when a key is pressed (as well as the Touche) allowing rapid articulation.


THE ONDES FRAMBOISE

The Ondes framboise is an attempt to create an affordable alternative to a genuine, extremely rare, Ondes Martenot or one of the almost as rare and very expensive alternatives. It uses digital sound synthesis by Pure Data (PD) running on a Raspberry Pi 3A+ to create all the waveforms from multiple wavetables driven by a single oscillator, and also attempts to emulate at least some of the character of the special diffuseurs entirely in software. A separate server process communicates with a custom hardware interface to read digital inputs from the keyboard and switches, an accelerometer physically attached to the keyboard for vibrato, and an 8-channel ADC which reads analogue input from the ribbon (using a 10-turn potentiometer) the Touche (using a pressure sensitive resistor), the level controls for the Octaviant, petit gambe and Souffle levels, the 'effects diffuseur' level and the Feutre and Expression pedals. PD and the server communicate with each other using OSC messages.

In addition to these 'conventional' Ondes controls, some additional features have been added. The prototype is based on a 4-octave full-size key keyboard (like some of the currently available Ondes intruments, e.g. the Ondomo), so the octave shift buttons have been modified to allow ±1 or 2 octaves shift (latching). In practice the lowest and highest few notes are not really usable, but the useful range is still >7 octaves. The ribbon is fitted with red/green LEDs to mark the position of the 'C' notes in red, with middle C in green to indicate the octave shift currently in force. The Touche button has an internal RGB LED to give visual feedback of the current volume level from off (silent) through red, yellow, green, cyan, blue, magenta to white (maximum volume) - particularly useful for claquement mode to give advance warning of the volume before a key is pressed. There is also a 2x16 LCD display with a rotary encoder for input, allowing for a number of additional options:
  - the tuning can be adjusted in steps of 0.1Hz. Both the main oscillator and all of the Palme resonators are tuned
  - the 'C' markers can be all on, only Middle C on, or all off
  - the Touche LED can be enabled or disabled
  - recording mode allows live or MIDI performances to be recorded directly to a 4-channel WAV file on the RPi SD card
  - a MIDI file on the RPi SD card can be played back and the audio recorded if required
  - the current tuning and LED configuration can be saved and will be loaded automatically at the next startup
  - the RPi OS can be updated without having to log in over WiFi
  - the RPi can be rebooted, or shutdown cleanly before poweroff

Early experiments with the Ondes framboise used a PiFi DAC+ 2-channel audio card, but the current version uses a MAYA44 USB+ 4-channel interface, allowing each diffuseur to have its own output channel via two class D stereo amplifiers. A recording of Rachmaninov's 'Vocalise' played on the Ondes framboise with accompaniment on a Casio Privia PX-350M (both recorded digitally) is available on YouTube (https://www.youtube.com/watch?v=8Qs7qE2W0aA). There is also a recording of the four 'Feuillets Inédits' by Olivier Messiaen (https://www.youtube.com/watch?v=9ope-bVgxdk&t=9s). Some sections of the Ondes part in the latter are beyond my limited capabilites and were recorded using the MIDI playback function.

The 3D printed parts and construction information (now essentially complete) are published on Thingiverse (https://www.thingiverse.com/thing:4742145).

LATEST UPDATES:
16-02-2021 - ondes_server_M.c code written to work with a USB keyboard instead of the switch-matrix keyboard of the original. MIDI input is handled in the server code so the Pure Data patch is unchanged. Works with a Korg microKey2-61 and an AKAI LPK25, so should be fine with the majority of MIDI keyboards.

14-02-2021 - updated the construction notes document on Thingiverse (see https://www.thingiverse.com/thing:4742145/files). Now includes a corrected layout for the interface board with (I think) all features working correctly - SN74HC595 shift register LED drivers, MCP3008 ADC, MCP23S08 port expander and ADXL632 accelerometer.

21-02-2021 - the new interface board and modified server code (ondes_server_M) has been tested in the Ondes with a Korg microKEY2-61 as the keyboard input and everything works as expected. I have yet to make the linear rail mountings for the Korg keyboard but this should be trivial for anyone capable of building an Ondes themselves.
