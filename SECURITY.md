# Reporting security issues

Use **Security → Report a vulnerability** on the ScreenFrame repository when private vulnerability reporting is enabled. Otherwise contact the maintainer through a private channel. Include the affected version, platform, and impact. Use synthetic screenshots and omit access tokens, SSH keys, and personal files.

ScreenFrame's library is designed for devices under the same administrator's control. The server listens on loopback and uses a shared bearer token. A device with the token can view the entire library. Give it only to devices you trust. Use authenticated SSH forwarding for access from another machine.

If a library token is exposed, generate a new token on the server, restart the service, and reconnect trusted clients. The old token stops working. Keep a private copy of unsent screenshots before changing a destination or token.
