# Project 1 - Simple Mail Client

- Name: Mackenzie Wright
- Email: mackenziewright@u.boisestate.edu
- Class: CS425-001

## Known Bugs or Issues

There are no known bugs or issues.

## Experience

This project was honestly a lot to understand and code. I have a  good understanding of
SMTP from previous classes I've taken, so I understood the bsaics of how this communication
protocol works. However, this is a much more in-depth look into the different ways a server 
can respond and how a TLS connection is created. This is important to know and has helped me 
create a better mental image into how this type of connection works and what it looks like 
over the internet. It took me a long time to understand how to go about this project without
using the already available TLS commands, meaning I had to add a lot of sanity checks to my
functions. This made it easier to write an in-depth testing suite because I checked
for all possible situations, but it also meant I had to write more tests to reach 100% 
coverage. Reaching 100% coverage took me a while even with asking AI to help generate 
potentially missed areas, but it helped that the coverage report lists exactly what lines
have been missed. Much of my time spent on this project was checking the specific requirements
and expected response for each functionality.

## Design
The protocol is split into three sections because we can't create unit tests that work with a 
live mail server. This means our logic must not use/touch a socket in order for us to write
our unit tests. To test functionality, I used a fake server to test connections. I split the 
protocol up into three section; pure protocol helpers, the session, and the socket transport. 
Pure protocol helpers are simply functions that take in strings and return either strings or status codes. There's absolutely no I/O present here and is responsible for parsing reply lines, dot stuffing a body, and building payload. The second section, the session, is essentially used to monitor and run the whole session. Its functionality includes reading in replies, writing, sending commands, and checking codes. This is done through a pair of read and write function pointers. The last section is responsible for overseeing the socket transport. It generates thin wrappers that satisfy the callbacks created in the session. 
