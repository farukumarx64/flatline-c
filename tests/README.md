# Tests

No automated tests exist yet. At this stage, verification consists of compiling
and running all three scaffold executables in normal and sanitizer builds.

Add behavior tests alongside each implemented feature. The first protocol tests
should cover header encoding and decoding, partial messages, invalid frames,
and connection closure before a complete message arrives. Integration tests
will later exercise job execution, worker failure, and coordinator recovery.
