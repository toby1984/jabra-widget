## Shows notifications for Jabra bluetooth headphone (Jabra Evolve 65 SE etc.) charging state when connected via the Jabra bluetooth dongle

Using the Jabra bluetooth dongle does not register the headphones as regular bluetooth device so tools that works for regular battery-powered bluetooth devices will _NOT_ work in this case. That's why I've written this program.
This is a Linux daemon written in C that uses Jabra's 3rd party library to periodically poll the battery status of connected Jabra devices and display it as a desktop notification every time the battery level has changed by 5% or the charging status changes.

Eventually going to turn this into a KDE widget... 
