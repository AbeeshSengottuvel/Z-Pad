# Z-Pad

Here's the whole thing start to finish on Windows.
First, install Python if you don't have it. Go to python.org, download the Windows installer, and on the first screen of the installer tick "Add python.exe to PATH" before clicking Install — that one checkbox saves a lot of trouble later. To confirm it worked, open Command Prompt (press the Windows key, type cmd, Enter) and run python --version; you should see a version number.
Then install the libraries the app needs. In that same Command Prompt:
pip install pyserial pycaw comtypes psutil requests
Next, find the Bluetooth COM port. Pair the ESP32 ("DeskDeck") in Windows Bluetooth settings, then run this to list serial ports:
python -m serial.tools.list_ports
Look for the DeskDeck/Bluetooth entry — you want the Outgoing one (you can also see it under Settings → Bluetooth → More Bluetooth settings → COM Ports tab). Note its name, like COM5.
Now open the file and set two things. Right-click deskdeck_companion.py → Open with → Notepad (or any editor). Change COM_PORT = "COM5" to whatever port you found, and set VLC_PASSWORD to the password you put in VLC's web interface (only matters if you want VLC control). Save.
Finally, run it. In Command Prompt, go to the folder where the file is and start it:
cd C:\path\to\the\folder
python deskdeck_companion.py
You'll see Connected on COM5 once it links up — and that's also the moment your Bluetooth shows "connected" and volume starts responding, because the script opening the port is what makes the link go live. Leave that window open while you use the remote; closing it stops the companion. If it just prints Waiting for COM5..., the port name is wrong or the ESP32 isn't paired/on.
One tip for later: once it's working, you can have it start automatically with Windows so you don't open the command window each time. Want me to show how to set that up, or help if you hit an error when you run it?
