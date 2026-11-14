# Reporting security issues

Report vulnerabilities privately through **Security → Report a vulnerability** on the GitHub repository. Include the version, platform and impact. Use synthetic screenshots and leave out access tokens, SSH keys and personal files.

ScreenFrame libraries use a shared access token. A device with the token and a route to the library can read every screenshot and upload new ones. The connection code contains the token, so share the code only with devices you trust.

The embedded library accepts HTTP on loopback. When you enable **Access from other devices**, it also listens on the computer's verified Tailscale address. ScreenFrame checks that the destination is a Tailscale peer and binds the outgoing connection to its own Tailscale address. Tailscale encrypts the traffic between devices on the private network. Keep the library port private and share it through Tailscale. [Tailscale connection types](https://tailscale.com/docs/reference/connection-types)

The standalone Python server accepts HTTP on loopback. To reach it from another device, use authenticated SSH forwarding.

If an embedded library's connection code is exposed, turn off **Access from other devices** and apply the change to stop remote access. For a standalone server, replace its token, restart the service, and reconnect trusted clients. Keep a private copy of unsent screenshots before changing a destination or token.
