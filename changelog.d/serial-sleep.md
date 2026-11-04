Fixed:
Serial and Wi-Fi remote `CMD:SLEEP` (debug builds) now puts the device to sleep the way the power key does. It used to draw the sleep screen and stay awake, with the main loop ticking every 10 ms.
