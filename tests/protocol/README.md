# Protocol tests

CTest covers header serialization, AU marker preservation, fragment reordering,
inconsistent flags, corrected retransmission and missing-fragment recovery.

Protocol v1 remains a 40-byte header. Flag bit0 marks IDR; bit1 marks a complete
H.264 access unit in the entire reassembled payload. Every fragment of that
payload must carry identical flags. Do not mark a slice or generic synthetic
payload as a complete AU. Missing fragments never produce a CompletedFrame.
