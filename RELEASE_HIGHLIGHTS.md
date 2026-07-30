- Updates Voice PE, Satellite1, ReSpeaker XVF3800, and S3 Box to firmware
  version 0.3.1.
- Adds secure `wss://` support using the ESP certificate authority bundle,
  including HTTPS reverse-proxy deployments with publicly trusted certificates.
- Accepts and normalizes bare hostnames and IP addresses, HTTP(S), WS(S), proxy
  path prefixes, trailing slashes, and the complete native WebSocket endpoint.
- Repairs previously saved compatible server-address forms automatically during
  startup, without requiring users to provision the satellite again.
- Rejects malformed server addresses with a clear setup error and keeps an
  invalid address or WebSocket startup failure from causing a reboot loop.
