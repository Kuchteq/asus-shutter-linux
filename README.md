## Simple kernel module for controlling the electronic camera shutter on ASUS UX5401ZA and its derivativs

It creates a special proc file that you can write to. 
echo 1 to cover the camera
` echo -n 1 | sudo tee /proc/acpi/asus-shutter; `
and echo 0 to uncover it
` echo -n 0 | sudo tee /proc/acpi/asus-shutter; `

This is largely based on the codebase from [acpi-call](https://github.com/mkottman/acpi_call) 

## TODO
Being able to retrieve the status (i.e. whether the shutter is on or off) by reading from the file
