/* The helper's identity, as HELLO reports it (multiboot-and-save.md §6.3). Only #defines and
 * comments: crt0.S includes this file too, for the header's version byte. */
#ifndef HP_VERSION_H
#define HP_VERSION_H

#define HP_PROTO_VERSION 1 /* also the header's version byte 0xBC; the Flipper checks it */
/* For logs and support only: the Flipper logs it and checks nothing, as it always sends its own
 * built-in image. Bump it when files/gen3_helper.mb changes after a release has shipped it. */
#define HP_BUILD_ID 1

#endif
