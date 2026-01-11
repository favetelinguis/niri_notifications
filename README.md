# Niri Layout Change Notifications

Sends out notifications on D-Bus each time you switch keyboard layout on Niri. Notifications will show up int for example mako, showing the language currently active. Monitoring is handled by systemd journal. Use journalctl to monitor logs. Uses the Niri socket event stream instead of polling.

## Requirements
- Systemd
- Niri

## Installation
make install


## Usage
Just us spawn_at_startup in Niri config since we require the NIRI_SOCKET to be set.
