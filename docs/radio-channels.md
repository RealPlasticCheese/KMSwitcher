# Radio channels and separate rigs

Each master has a saved radio channel, initially 6. Its receivers retain their
paired master MAC/key and remember the last working channel. The same firmware
can serve an everyday rig and a separate test rig without rebuilding for each
channel. Channels share the 2.4 GHz band; a different channel reduces airtime
competition but does not guarantee immunity to nearby transmitters.

## Upgrade an existing rig

1. Install the new master firmware and keep its channel at 6 (or the existing
   compiled channel for a customized older rig).
2. Pair spare S2s normally, one slot at a time. Older receivers can pair and run
   on that channel because the HID and OTA protocols are unchanged.
3. [OTA-update every paired S2](slave-management.md#updating-one-or-all-receivers)
   with the matching receiver `.bin` and `.json`.
   The master learns channel-discovery support from authenticated receiver
   queries. A channel change is refused until every paired receiver has
   demonstrated support. Reload radio status after receivers restart.
4. Finish updates before changing channels. The firmware blocks changes during
   pairing, retained test mode and the firmware-transfer session. After a
   transfer, the guard remains for up to two minutes after the final OTA command.
5. Open **Radio** in the companion, select a channel and save it.
   The allowed choices come from the compiled country policy (AU: 1–13).
   Everyday preferences/defaults do not reset this separate setting.

Keep a separate set of receivers paired to each master. Pairing windows should
be opened on only the intended master. For nearby rigs, consider 1, 6 and 11
when selecting separated channels; choose according to local interference.

## Companion app

Open the **Radio** tab in the companion app to load the master's current channel,
the channels allowed by its country policy, and the number of paired dongles that
support channel discovery. Select a channel and choose **Save channel**. This uses
the local receiver connection; the master does not need a serial connection to
the PC. Both the master and the local receiver need firmware supporting companion
channel commands, and every paired receiver must support channel discovery.

The app refreshes status after the switch. Input stays paused: allow the dongles
to reconnect, then choose **Resume routing**. Use **Refresh** if the radio link was
temporarily unavailable. If a save times out, refresh before retrying: the master
may already have accepted it. Busy pairing, testing and firmware-transfer guards
apply just as they do to serial commands. Pairings are retained.

## Automatic channel recovery

In **Radio**, use **Automatic channel recovery** and choose **Save recovery
settings**. The master saves the settings for the whole rig. Update the master
and receiver firmware before enabling this feature.

- **Try a different channel when delivery stays poor** is off by default.
- **Allow a trial with one reachable dongle** is on by default. The master
  requires two consecutive 30-second windows of sustained MAC delivery faults
  when a single dongle is reachable, and any eligible slave restart takes
  priority. With several reachable dongles, at least two links must show
  sustained faults independently.
- **Time between trials** defaults to 15 minutes and accepts 5–60 minutes.
  The master allows at most two trials per hour. The attempt budget and the
  recently tried channels survive a master restart.

The master probes paired, recently heard dongles even when they are inactive
or their USB hosts are asleep. It uses authenticated requests and gives
keyboard and mouse input first use of the transmitter. It sends at most one
probe per second across the desk and at most one every three seconds per
dongle. A slow or failed MAC completion extends the desk-wide interval to
2.2 seconds. Dongles out of radio contact for six seconds age out of the next
trial's expected set. They can rediscover the current channel when they return.

A window needs at least three usable probes across at least five seconds.
For multiple links, at least half the probes on each affected link must show
slow or failed delivery, with at least one slow or failed MAC completion.
The single-link rule requires MAC evidence for at least half the probes in
each of its two windows. The initial thresholds are 45 ms for MAC completion
and 55 ms for round-trip time after subtracting receiver turnaround. Processing
delays of 30 ms or more and recent receiver USB stalls exclude a sample.
Signal strength is available in diagnostics. Delivery measurements drive the
decision.

A trial chooses a permitted channel and pauses routing while the master
compares the same probes against its baseline. It allows up to 32 seconds
after retuning for the expected dongles to reconnect and verify delivery.
Each expected dongle must reply to at least two probes, with at most 25%
slow or failed delivery and at most 25% unanswered probes. Affected links
must reply to at least three probes and improve by at least 25 percentage
points. The master saves the candidate
only after verification. If verification fails, the master returns to the
previous channel and allows up to 20 seconds for rediscovery. Routing resumes
when the selected dongle reports fresh USB readiness. Input from the pause
interval is discarded.

Pairing, firmware transfers, test mode, and slave reboot or confirmation
defer trials. Receiver reboot recovery uses fresh evidence after the channel
operation and a 12-second guard. Manual channel changes take over from an
automatic trial. A manual pause or disabling automatic recovery cancels
automatic route restoration. A manual selection made during a trial takes
effect when that dongle reconnects after rollback.

The Radio tab shows trial progress, the most recent result and the attempt
count. The master's buffered UART log records **KMSCHANNELRECOVERY** events
with the reason, channel pair, affected dongles, before and after measurements
and rollback outcome. Check the observation thresholds under normal input and
Companion load on the rig before routine use.

## UART control

```
channel
channel get 101
channel set 102 11
```

The numeric ID correlates a response with a request. `channel set 103 6` explicitly
saves channel 6. Responses have this format:

```
KMSCHANNEL ID RESULT CURRENT PENDING FIRST COUNT PAIRED SUPPORTED
```

`RESULT` is `OK`, `INVALID`, `BUSY`, `STORAGE` or `UPGRADE_SLAVES`. `CURRENT` and
`PENDING` differ during a switch. Check again after two seconds. `SUPPORTED`
must equal `PAIRED` before changing to a different channel. Previously confirmed
support is retained by slot/MAC so an upgraded receiver can be offline during a
later change. Downgrading such a receiver to old fixed-channel firmware removes
its ability to follow the master; keep both roles updated.

Changing channel pauses routing, discards unsent input and informs reachable
receivers of the target channel and remaining delay. The master switches after
1.8 seconds. It stays paused; wait for receivers to reconnect, then use **Resume**
or `resume`. Pairings and the previous route are retained. A failed radio retune
keeps the old channel and attempts to restore its saved setting.

## Discovery and recovery

Updated receivers send small authenticated channel queries while their normal
HID/management protocol remains unchanged. Requests and replies use the paired
key, HMAC-SHA256 with a 128-bit tag, a receiver boot ID and a fresh challenge.
A receiver saves a discovered channel only after validating the reply to its
outstanding challenge. Another nearby master cannot claim it.

After four seconds without discovery or validated HID/management contact, a paired receiver releases held
input and tries each permitted channel for 400 ms, retaining its master identity
and key. An offline receiver, or one that missed the switch notice, therefore
finds its master after returning. Recovery can take several seconds; there is
no lossless-input promise while changing channels. OTA writes freeze searching.

An unpaired receiver stays on its starting channel until its physical pairing
window is opened. During that window it searches channels; a pairing candidate
freezes searching until the exchange completes or times out. Successful pairing
saves the channel; an unsuccessful window returns to the prior saved channel.
Upgrade the master first. Older OTA-capable masters can maintain contact through
their existing management heartbeat, but do not advertise or coordinate channel
changes. Only upgraded masters and receivers support moving a rig to a new channel.

Factory reset clears the channel and pairings together and restores the compiled
default. Ordinary firmware updates preserve them. The compiled
`KMS_RADIO_CHANNEL` now supplies the initial default rather than forcing every
board to channel 6.
