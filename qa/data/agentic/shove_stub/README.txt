The eco_logger board of the third scripted tool test, before U2.6 -> J2.2 was routed. That route
needs shove mode, and the shove broke the +5V track next to J2.2 into two pieces that led nowhere
(the net stayed connected another way); pcb_route now removes such copper (test_tool_findings.py).
