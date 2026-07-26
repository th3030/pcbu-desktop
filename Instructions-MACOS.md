# PC Bio Unlock for macOS - Installation Guide

> **⚠️ Warning (Preview Build)**
>
> PC Bio Unlock integrates with macOS authentication using PAM and Authorization Services. Incorrect configuration or uninstalling the application without restoring the original PAM and authorization settings can cause you to be locked out of your Mac. Before installing, ensure you know how to access macOS Recovery Mode. I am not responsible if you're locked out of your Mac!

## Requirements

* Apple Silicon Mac (ARM64)
* Administrator account
* macOS supported by this release
* Comfortable using the Terminal
* Fast User Switching enabled
* A valid `/usr/local/sbin` directory
* A valid `/usr/local/lib/pam` directory

## Should I use PC Bio Unlock on Mac?
For most users, **Touch ID is the recommended authentication method on macOS.**

---

# Installation

## 1. Install the application

1. Open the downloaded **PCBioUnlock.dmg**.
2. Drag **PCBioUnlock.app** into the **Applications** folder.
3. Eject the DMG.

---

## 2. Install OpenSSL Libraries

PC Bio Unlock depends on the **OpenSSL libraries** (`libssl` and `libcrypto`).
Without this you may get locked out if PC Bio Unlock has installed the modules.

### Using Homebrew (Recommended)

If Homebrew is not installed, install it first.
Open the terminal and enter this command:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

Install the OpenSSL libraries:

```bash
brew install openssl
```

After installation, Homebrew will install the required libraries in its default location. No additional configuration is normally required.

## 3. Prepare the system

### 3.1 Enable Fast User Switching

PC Bio Unlock uses the macOS login window authentication flow. To allow the lock screen to transition back to this authentication flow, **Fast User Switching must be enabled**.

Enable Fast User Switching:

1. Open **System Settings**.
2. Go to **Control Center**.
3. Find **Fast User Switching** in Users & Groups or in Menubar in newer versions.
4. Check **Fast User Switching** option. You may keep it on Icon

Restart the Mac or log out and back in for the changes to apply.

> **Important:** Without Fast User Switching enabled, the **Other Users** option may not appear on the lock screen. PC Bio Unlock may not be reachable from the macOS screensaver unlock flow.

### 3.2 Check required directories

PC Bio Unlock requires a valid `/usr/local/sbin` directory for `pcbu_auth`.

If `/usr/local/sbin` does not already exist on your system, create it before continuing:

```bash
sudo mkdir -p /usr/local/sbin
```

PC Bio Unlock also requires `/usr/local/lib/pam` to exist.

If `/usr/local/lib/pam` does not already exist on your system, create it before continuing:

```bash
sudo mkdir -p /usr/local/lib/pam
```

PC Bio Unlock can be easier invoked if you change the macOS authorization settings to route the lock screen through the login window via black dialog screen.
To enable this behavior, run:

```bash
sudo security authorizationdb write system.login.screensaver authenticate-session-owner
```

This changes the screensaver unlock behavior so it uses the session owner authentication path.

---

## 4. Launch PC Bio Unlock

> **Note:** When downloading unsigned builds, macOS may quarantine the application. If the application refuses to start, remove the quarantine attribute before launching:
>
> ```bash
> sudo xattr -dr com.apple.quarantine /Applications/PCBioUnlock.app
> ```

---

PC Bio Unlock requires elevated privileges.

Open **Terminal** and execute:

```bash
sudo /Applications/PCBioUnlock.app/Contents/MacOS/pcbu_desktop
```

Enter your administrator password when prompted.

## 5. Install the authentication modules

After the application starts:

1. Open the **Desktop UI Installer**.
2. Complete the installation process.
3. Restart the application if requested.

---

## 6. Enable integrations

Open **Settings** inside PC Bio Unlock and enable:

* ✅ Enable **Enable sudo Integration**
* ✅ Enable **Enable macOS Integration**

These integrations configures the required PAM modules.

---

# Verifying the installation

After installation:

* PC Bio Unlock should start successfully.
* `sudo` authentication should invoke PC Bio Unlock.
* The macOS screensaver unlock should display a black authentication screen.
* The **Other Users** button should be available.
* Selecting **Other Users** should open the login window.
* Pressing **Enter** in the login window should invoke PC Bio Unlock.
* When you turn on the Mac and log in to a PC Bio Unlock enabled account. **ALWAYS** press Ctrl + Option when the loading bar is filled to a quarter to prevent the login screen from hanging forever

---

# Uninstalling

> **⚠️ Important**
>
> Do **not** simply delete the application.
>
> PC Bio Unlock modifies PAM configuration and macOS authorization settings. Removing the application without restoring the original PAM files and authorization database entries can leave macOS referencing missing PAM modules or altered authentication behavior, potentially preventing authentication.

## Step 1

Open the PC Bio Unlock app via:

```bash
sudo /Applications/PCBioUnlock.app/Contents/MacOS/pcbu_desktop
```

## Step 2
Enter the settings in PC Bio Unlock app and uncheck:

*  **Enable sudo Integration**
*  **Enable macOS Integration**

## Step 3

Verify that neither file references the PC Bio Unlock PAM module.

## Step 4

Restore the macOS screensaver authorization setting:

```bash
sudo security authorizationdb reset system.login.screensaver
```

This returns the screensaver unlock behavior to the default macOS configuration.

## Step 5

Delete the application by bringing it to the Trash and emptying the Trash

## Step 6

Restart your Mac. Or logoff and login again to see the changes.

## Step 7 (Optional)

If OpenSSL was installed only for PC Bio Unlock, it can be removed together with Homebrew.

> **Warning**
>
> Removing Homebrew will uninstall all packages installed through Homebrew. Verify that no other applications depend on Homebrew before continuing.

### 1. Remove OpenSSL

First, remove the OpenSSL package:

```bash
brew uninstall openssl
```

Verify that OpenSSL has been removed:

```bash
brew list | grep openssl
```

If no output is returned, OpenSSL has been removed successfully.

---

### 2. Remove unused Homebrew packages

Clean up remaining unused files:

```bash
brew autoremove
brew cleanup
```

---

### 3. Remove Homebrew (Optional)

If Homebrew is no longer needed, uninstall Homebrew:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/uninstall.sh)"
```

Confirm the uninstall when prompted.

---

### 4. Verify removal

Check whether Homebrew is still installed:

```bash
brew --version
```

If Homebrew has been removed, the terminal should report that the command cannot be found.

---

# Recovery

If you removed the application before restoring the PAM configuration or authorization settings, macOS may no longer authenticate correctly.

Symptoms may include:

* Authentication always fails.
* The login screen repeatedly returns after entering the correct password.
* `sudo` no longer works.
* The screensaver unlock flow behaves unexpectedly.

In this situation:

1. Boot into **macOS Recovery**.
2. Open **Terminal**.
3. Remount **Macintosh HD** so the system volume is writable and you can reset the PAM configuration files.
4. Restore or edit the PAM configuration files to remove all PC Bio Unlock references.
5. Restart the Mac.

---

# Known limitations

* PC Bio Unlock requires administrator privileges to operate.
* This preview version uses PAM integration and macOS authorization database changes.
* Future versions may change the installation and authentication architecture.
* The lock screen may stay black indefinity. You have to hard reset the Mac (holding the power button)
