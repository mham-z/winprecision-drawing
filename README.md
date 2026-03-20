### Use your Windows Precision Touchpad as a drawing pad for handwriting, signatures, art, and more.
---
## Help
### Before you use this
It should probably go without saying that this works on Microsoft Windows *only*, with an available Precision Touchpad. So far I have only tested this on Windows 10 with an Acer Swift 1.

Do not expect to see any fancy features like palm rejection or hovering being visible.

### How to use
Run the app. Instructions will be given to you in your terminal.

Enter drawing mode by using [CTRL] + [SUPER] to mark 2 points of the area you want to draw in.

Use [ALT] + [SUPER] to enter drawing mode using the last area you marked.

Help option output:
```
winprecision-drawing [option]
Options:
	--help / -h     : List the available options.
	--shift-penup   : Raise pen when [SHIFT] is used. (Default)
	--shift-pendown : Lower pen when [SHIFT] is used.
	--shift-none    : [SHIFT] is ignored.
```
## Building
### Prerequisites
You must have GCC available, and must be able to use the options `-lhid -lgdi32`

### Build
Run `./build.bat` and GCC will produce `binary.exe` in the same folder.

## Disclaimer
[winprecision-drawing](.) is a fork and edit of [arpruss/finger-draw](https://github.com/arpruss/finger-draw), which is under the MIT License.
This repository is not labeled as a fork because I did not use GitHub's fork feature.
