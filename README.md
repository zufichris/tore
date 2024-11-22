# Tore (TOtall REcall)

> [!WARNING]
> The project is designed for my personal needs only, and may not be suitable for everybody. I do not offer any support for it either. It's completely Open Source, fork it and adapt it to your needs.

## Quick Start

```console
$ cc -o nob nob.c
$ ./nob
$ sudo cp ./build/tore /usr/local/bin/
$ echo "tore" >> ~/.bashrc
```

## Notifications vs Reminders

Notifications and Reminders are the two cornerstones of the Tore
paradigm and the difference between them is very important.

Notifications are your "Mailbox". You receive them so you can recall
the Thing. Notifications stay in your Mailbox until you explicitly
dismiss them.

Reminders are the generators of the Notifications. You schedule
Reminders to "fire off" Notifications into your Mailbox. Reminders can
also be configured to fire off Notifications periodically (every N
days, every N months, every N years, etc).

Just running `tore` in the terminal fires off all the necessary
Notifications and shows your Mailbox. This is why you put it into your
`.bashrc`, so you can always recall the Thing every time you open your
Terminal (and you open your Terminal every single day multiple times,
because you are a huge nerd).

For more information on how to manipulate Notifications and Reminders
run `tore help`.
