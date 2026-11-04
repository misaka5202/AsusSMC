//
// SSDT-Fan.dsl  —  ASUS ROG Zephyrus G15 GA503QM (Ryzen 7 5800HS)
//
// Adds Darwin-only helpers that return REAL fan RPM by going through the
// firmware's own ASUS WMI (ATKD.WMNB) path, instead of EC0.TACH().
//
// Why not TACH: EC0.TACH() computes 0x0041CDB4 / (raw * 2). With the raw
// values this board actually returns (0x17..0x22 = 23..34) that yields
// ~59000..93750 "rpm", which is nonsense. ATKD.WMNB(DSTS, 0x00110013) returns
// 0x00010000 | raw and both mainline asus-wmi (fan1_input) and AsusSMC's own
// WMI fallback read it as raw * 100 RPM -> 2300..3400. That matches reality.
// Verified on the machine: full speed gives 6800.
//
// Nothing here runs unless _OSI("Darwin"), so a dual-boot Windows install is
// untouched.
//
// Build:  iasl SSDT-Fan.dsl   ->  SSDT-Fan.aml
//

DefinitionBlock ("", "SSDT", 2, "MISK", "FANGA503", 0x00000001)
{
    External (_SB_.PCI0.SBRG.EC0, DeviceObj)
    External (_SB_.PCI0.SBRG.EC0.ECAV, MethodObj)    // 0 args
    External (_SB_.ATKD.WMNB, MethodObj)    // 3 args

    Scope (\_SB.PCI0.SBRG.EC0)
    {
        // ---------------------------------------------------------------
        // FANQ (id) -> raw 16-bit fan counter, straight from the firmware.
        //   0 = CPU fan     (DSTS/DEVS device 0x00110013)
        //   1 = second fan  (DSTS/DEVS device 0x00110014)
        // Returns 0xFFFFFFFF on failure.
        // ---------------------------------------------------------------
        Method (FANQ, 1, Serialized)
        {
            If (!_OSI ("Darwin"))
            {
                Return (0xFFFFFFFF)
            }

            If (!ECAV ())
            {
                Return (0xFFFFFFFF)
            }

            Local0 = 0x00110013
            If ((Arg0 == One))
            {
                Local0 = 0x00110014
            }
            ElseIf ((Arg0 != Zero))
            {
                Return (0xFFFFFFFF)
            }

            // WMNB(Arg0=0, MethodID, Arg2). Arg2 must hold IIA0..IIA4 over
            // bytes 0x00..0x13 (the method does CreateDWordField on it), so
            // 20 bytes are mandatory — Linux passes 24 for the same reason.
            Local1 = Buffer (0x14)
            {
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00
            }
            Local1 [Zero] = (Local0 & 0xFF)
            Local1 [One] = ((Local0 >> 0x08) & 0xFF)
            Local1 [0x02] = ((Local0 >> 0x10) & 0xFF)
            Local1 [0x03] = ((Local0 >> 0x18) & 0xFF)

            Return (\_SB.ATKD.WMNB (Zero, 0x53545344, Local1))   // DSTS
        }

        // ---------------------------------------------------------------
        // FAN0 / FAN1 -> fan RPM as a plain integer (0 if unreadable).
        // ---------------------------------------------------------------
        Method (FAN0, 0, Serialized)
        {
            Return (FANR (Zero))
        }

        Method (FAN1, 0, Serialized)
        {
            Return (FANR (One))
        }

        Method (FANR, 1, Serialized)
        {
            Local0 = FANQ (Arg0)
            If ((Local0 == 0xFFFFFFFF))
            {
                Return (Zero)
            }

            Local0 &= 0xFFFF
            If ((Local0 == Zero))
            {
                Return (Zero)
            }

            Local0 *= 0x64        // firmware unit is 1/100 RPM
            Return (Local0)
        }

        // ---------------------------------------------------------------
        // FSPD (id, on) -> force one fan full speed / back to automatic.
        //   on nonzero -> set the 0x40 bit in EC page 0xCC:0x30 (full speed)
        //   on zero    -> clear it, firmware curve takes over again
        // This is the same path AsusSMC/asus-wmi take for the CPU fan ctrl
        // device. Verified reversible on hardware (6800 -> 2400 RPM).
        // ---------------------------------------------------------------
        Method (FSPD, 2, Serialized)
        {
            If (!_OSI ("Darwin"))
            {
                Return (Zero)
            }

            If (!ECAV ())
            {
                Return (Zero)
            }

            Local0 = 0x00110013
            If ((Arg0 == One))
            {
                Local0 = 0x00110014
            }
            ElseIf ((Arg0 != Zero))
            {
                Return (Zero)
            }

            Local1 = Buffer (0x14)
            {
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00
            }
            Local1 [Zero] = (Local0 & 0xFF)
            Local1 [One] = ((Local0 >> 0x08) & 0xFF)
            Local1 [0x02] = ((Local0 >> 0x10) & 0xFF)
            Local1 [0x03] = ((Local0 >> 0x18) & 0xFF)
            Local1 [0x04] = Zero
            If (Arg1)
            {
                Local1 [0x04] = One
            }

            Return (\_SB.ATKD.WMNB (Zero, 0x53564544, Local1))   // DEVS
        }
    }
}
