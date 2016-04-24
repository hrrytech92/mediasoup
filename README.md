![mediasoup][mediasoup_banner]


**Status:** Coming soon (check the project [milestones](https://github.com/ibc/mediasoup/milestones)).


## Design goals

* Be a Node.js library: `npm install mediasoup`
* Be minimalist: just handle the media layer
* Don't deal with network signaling protocols (SIP, XMPP, etc)
* Expose a modern ECMAScript 6 [API](/api/) in sync with [ORTC](http://ortc.org/)
* Work with current WebRTC client implementations


## Features

* Multiple conference rooms with multiple participants
* IPv6 ready
* ICE / DTLS / RTP / RTCP / DataChannel over UDP and TCP
* Extremely powerful (media handler subprocess coded in C++ on top of [libuv](http://libuv.org))
* Can handle RTP packets in JavaScript land
* "ICE Lite" implementation (**mediasoup** must run in a publicly reachable host)


## Requirements

* Node.js >= `v4.0.0`
* POSIX based operating system (Windows not yet supported)


## Installation

```bash
$ npm install mediasoup --save
```


## Author

Iñaki Baz Castillo ([@ibc](https://github.com/ibc/) at Github)


## License

[ISC](./LICENSE)


[mediasoup_banner]: https://raw.githubusercontent.com/ibc/mediasoup-website/master/_art/mediasoup_banner.png
