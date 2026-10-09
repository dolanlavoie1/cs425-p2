# Project 2

- Name: Dolan Lavoie
- Email: dolanlavoie@u.boisestate.edu
- Class: CS425-001

## Known Bugs or Issues

None

## Experience

This was an interesting project because it was cool to get to send packets between a sender and a reciever, and have them communicate with each other, simiarly to how real tcp communication works. One of the hardest parts was understandings all of the concepts and the things that the sender and reciever have to do, that way I could explain it to the AI, and know if it makes any errors. Something I did notice wile using copilot on this project is that it performed much better at the begining of the project than the end. Towards the end it starting doing some stupid things, like naming functions strangely , putting things in main.c that actually should have been in lab.c, and excluding over half of lab.c from testing, when it could be tested. I found the activity at the end to be interesting, especially seeing how a packet loss is more impactful on a transfer with a higher window than a lower one. it was also interesting to see how much of a difference that changing the window size makes. 

## Analysis

| Window | Loss | Mean Time | Mean Throughput |
| ------ | ---- | ---- | ---------- |
| 1      | 0    | 106.374s | 9.63 KiB/s |
| 1      | 0.05 | 135.677s | 7.58 KiB/s |
| 16     | 0    | 6.801s | 150.57 KiB/s |
| 16     | 0.05 | 21.511s | 47.6 KiB/s |

1. In the window 1 loss 0 run, 1025 packets were sent at 100ms round trip time each. This comes out to 102.5 seconds of round trip time, with about 3.9 seconds of time where we weren't waiting for packets to be sent. That time is made up of the server starting up, the server waiting a few seconds to close after the FIN, and other overhead from python, the c programs, and possibly wsl. 
2. The speedup between a window of 1 and a window of 16 with a loss of 0 is about a 16x speed improvement because it is able to send 16 packets concurrently, while a window of 1 has to wait the entire 100ms between packets before it can send another. 
3. The 0.05 loss hurts a window of 16 more than a window of 1 because when a packet is lost, all packets above it have to be retransmited. This means that for a packet of 1, if the packet is lost only that one packet has to be resent. However, in a window of 16, if a packet is lost then up to 16 packets might have to be sent again. 