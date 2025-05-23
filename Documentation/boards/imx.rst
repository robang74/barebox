Freescale i.MX
==============

Freescale i.MX is traditionally very well supported under barebox.
Depending on the SoC, there are different Boot Modes supported. Older
SoCs up to i.MX31 support only the external Boot Mode. Newer SoCs
can be configured for internal or external Boot Mode with the internal
boot mode being the more popular mode. The i.MX23 and i.MX28, also
known as i.MXs, are special. These SoCs have a completely different
boot mechanism, see :doc:`mxs` instead.

Internal Boot Mode
------------------

The Internal Boot Mode is supported on:

* i.MX25
* i.MX35
* i.MX50
* i.MX51
* i.MX53
* i.MX6
* i.MX7
* i.MX8MQ

With the Internal Boot Mode, the images contain a header which describes
where the binary shall be loaded and started. These headers also contain
a so-called DCD table which consists of register/value pairs. These are
executed by the Boot ROM and are used to configure the SDRAM. In barebox,
the i.MX images are generated with the ``scripts/imx/imx-image`` tool.
Normally it's not necessary to call this tool manually, it is executed
automatically at the end of the build process.

Required entries for an i.MX image in ``images/Makefile.imx`` are for example:

.. code-block:: none

  pblb-$(CONFIG_MACH_MYBOARD) += start_imx6dl_myboard
  CFG_start_imx6dl_myboard.pblb.imximg = $(board)/myboard/flash-header-imx6dl-myboard.imxcfg
  FILE_barebox-imx6dl-myboard.img = start_imx6dl_myboard.pblb.imximg
  image-$(CONFIG_MACH_MYBOARD) += barebox-imx6dl-myboard.img

The first line defines the entry function of the pre-bootloader.
This function must be defined in the board's ``lowlevel.c``.
The second line describes the flash header to be used for the image, which is
then compiled into an imximg file.
The prebootloader is then added to the final barebox image.

The images generated by the build process can be directly written to an
SD card:

.. code-block:: sh

  # with Multi Image support:
  cat images/barebox-freescale-imx51-babbage.img > /dev/sdd
  # otherwise:
  cat barebox-flash-image > /dev/sdd

The above will overwrite the MBR (and consequently the partition table)
on the destination SD card. To preserve the MBR while writing the rest
of the image to the card, use:

.. code-block:: sh

  dd if=images/barebox-freescale-imx51-babbage.img of=/dev/sdd bs=1024 skip=1 seek=1

Note that MaskROM on i.MX8 expects the image to start at the +33KiB mark, so the
following command has to be used instead:

.. code-block:: sh

  dd if=images/barebox-nxp-imx8mq-evk.img of=/dev/sdd bs=1024 skip=33 seek=33

Or, in case of NAND:

.. code-block:: sh

  dd if=images/barebox-nxp-imx8mq-evk.img of=/dev/nand bs=1024 skip=33 seek=1

The images can also always be started as second stage on the target:

.. code-block:: console

  barebox@Board Name:/ bootm /mnt/tftp/barebox-freescale-imx51-babbage.img

BootROM Reboot mode codes (bmode)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

For selected SoCs, barebox supports communicating an alternative boot medium
that the BootROM should select after a warm reset::

  barebox@FSL i.MX8MM EVK board:/ devinfo gpr.reboot_mode
  Driver: syscon-reboot-mode
  Bus: platform
  Parent: 30390000.reset-controller@30390000.of
  Parameters:
    next: normal (type: enum) (values: "normal", "serial")
    prev: normal (type: enum) (values: "normal", "serial")
  Device node: /soc@0/bus@30000000/reset-controller@30390000/reboot-mode
  reboot-mode {
          compatible = "barebox,syscon-reboot-mode";
          offset = <0x94 0x98>;
          mask = <0xffffffff 0x40000000>;
          mode-normal = <0x0 0x0>;
          mode-serial = <0x10 0x40000000>;
  };

  barebox@FSL i.MX8MM EVK board:/ gpr.reboot_mode.next=serial reset -w

The example above will cause barebox to jump back into serial download mode on
an i.MX8MM by writing 0x10 into the *SRC_GPR9* register (offset 0x30390094) and
0x40000000 into the *SRC_GPR10* register (offset 0x30390098), and then issuing a
warm :ref:`reset <command_reset>`.

Different SoCs may have more possible reboot modes available.
Look for documentation of the *SRC_SBMR* and *SRC_GPR* registers in the
Reference Manual of your SoC; the values for the ``mode-*`` properties often
correspond directly to the boot fusemap settings.

See the section on :ref:`Reboot modes<reboot_mode>` for general information.

High Assurance Boot
^^^^^^^^^^^^^^^^^^^

HAB is an NXP ROM code feature which is able to authenticate software in
external memory at boot time.
This is done by verifying signatures as defined in the Command Sequence File
(CSF) as compiled into the i.MX boot header.

barebox supports generating signed images, signed USB images suitable for
*imx-usb-loader* and encrypted images.

In contrast to normal (unsigned) images booting signed images via
imx-usb-loader requires special images:
DCD data is invalidated (DCD pointer set to zero), the image is then signed and
afterwards the DCD pointer is set to the DCD data again (practically making
the signature invalid).
This works because the imx-usb-loader transmits the DCD table setup prior to
the actual image to set up the RAM in order to load the barebox image.
Now the DCD pointer is set to zero (making the signature valid again) and the
image is loaded and verified by the ROM code.

Note that the device-specific Data Encryption Key (DEK) blob needs to be
appended to the image after the build process for appropriately encrypted
images.

In order to generate these special image types barebox is equipped with
corresponding static pattern rules in ``images/Makefile.imx``.
Unlike the typical ``imximg`` file extension the following ones are used for
these cases:

* ``simximg``: generate signed image
* ``usimximg``: generate signed USB image
* ``esimximg``: generate encrypted and signed image

The imx-image tool is then automatically called with the appropriate flags
during image creation.
This again calls Freescale's Code Signing Tool (CST) which must be installed in
the path or given via the environment variable "CST".

Assuming ``CONFIG_HAB`` and ``CONFIG_HABV4`` are enabled the necessary
keys/certificates are expected in these config variables (assuming HABv4):

.. code-block:: none

  CONFIG_HABV4_TABLE_BIN
  CONFIG_HABV4_CSF_CRT_PEM
  CONFIG_HABV4_IMG_CRT_PEM

A CSF template is located in
``include/mach/imx/habv4-imx6-gencsf.h`` which is preprocessed
by barebox.
It must be included in the board's flash header:

.. code-block:: none

  #include <mach/imx/habv4-imx6-gencsf.h>

Analogous to HABv4 options and a template exist for HABv3.

Secure Boot on i.MX6
~~~~~~~~~~~~~~~~~~~~

For most boards, the secure boot process on i.MX6 consist of the following image
constellation::

    0x0 +---------------------------------+
        | Barebox Header                  |
  0x400 +---------------------------------+       -
        | i.MX IVT Header                 |       |
        | Boot Data                       +--+    |
        | CSF Pointer                     +--|-+  | Signed Area
        +---------------------------------+  | |  |
        | Device Configuration Data (DCD) |  | |  |
 0x1000 +---------------------------------+  | |  |
        | Barebox Prebootloader (PBL)     |<-+ |  |
        +---------------------------------+    |  |
        | Piggydata (Main Barebox Binary) |    |  |
        +---------------------------------+    |  -
        | Command Sequence File (CSF)     |<---+
        +---------------------------------+

Here the Command Sequence File signs the complete Header, PBL and piggy data
file. This ensures that the whole barebox binary is authenticated. This is
possible since the DDR RAM is configured using the DCD and the whole DDR memory
area can be used to load data onto the device for authentication.
The boot ROM loads the CSF area and barebox into memory and uses the CSF to
verify the complete barebox binary.

Boards which do require a boot via SRAM, need changes akin to the implementation
for i.MX8MQ described in the next chapter.

Secure Boot on i.MX8MQ
~~~~~~~~~~~~~~~~~~~~~~

For i.MX8MQ the image has the following design::

    0x0 +---------------------------------+
        | Barebox Header                  |
        +---------------------------------+
        | i.MX IVT Header                 |
        | HDMI Firmware (Signed by NXP)   |
        +---------------------------------+        -
        | i.MX IVT Header                 |        |
        | Boot Data                       +--+     |
        | CSF Pointer                     +--|-+   |
        +---------------------------------+  | |   | Signed Area
        | Device Configuration Data (DCD) |  | |   |
        +---------------------------------+  | |   |
        | Barebox Prebootloader (PBL)     |<-+ |   |
        | Piggydata Hash (SHA256)         +----|-+ |
        +---------------------------------+    | | -
        | Command Sequence File (CSF)     |<---+ |
        +---------------------------------+      | -
        | Piggydata (Main Barebox Binary) |<-----+ | Hashed Area
        +---------------------------------+        -

In contrast to i.MX6, for the i.MX8MQ the piggydata can not be signed together
with the PBL binary. The DDR memory is initialized during the start of the PBL,
previous to this no access to the DDR memory is possible. Since the Tightly
Coupled Memory used for early startup on i.MX8MQ has only 256Kib, the whole
barebox can't be loaded and verified at once, since the complete barebox with
firmware has a size of ~500Kib.

The bootrom loads the HDMI firmware unconditionally, since it is signed by NXP.
Afterwards the Prebootloader (PBL) is loaded into SRAM and the bootrom proceeds
to verify the PBL according to the Command Sequence File (CSF). The verified
PBL initializes the ARM Trusted Firmware (TF-A) and DDR RAM. It subsequently
loads the piggydata from the boot media and calculates the sha256sum of the
piggydata. This is compared to the sha256sum built into the PBL during compile
time, the PBL will only continue to boot if the sha256sum matches the builtin
sha256sum.

Using GPT on i.MX
^^^^^^^^^^^^^^^^^

For i.MX SoCs that place a vendor specific header at the +1KiB mark of a
boot medium, special care needs to be taken when partitioning that medium
with GPT. In order to make room for the i.MX boot header, the GPT Partition
Entry Array needs to be moved from its typical location, LBA 2, to an
offset past vendor specific information. One way to do this would be
to use the ``-j`` or ``--adjust-main-table`` option of ``sgdisk``. For
example, the following sequence

.. code-block:: sh

  sgdisk -Z <block device>
  sgdisk -o -j 2048 -n 1:8192:+100M <block device>

will create a single GPT partition starting at LBA 8192 and would
place the Partition Entry Array starting at LBA 2048, which should leave
enough room for the Barebox/i.MX boot header. Once that is done, the ``dd``
command above can be used to place Barebox on the same medium.

Information about the ``imx-image`` tool
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The imx-image tool can be used to generate imximages from raw binaries.
It requires an configuration file describing how to setup the SDRAM on
a particular board. This mainly consists of a poke table. The recognized
options in this file are:

Header:

+--------------------+--------------------------------------------------------------+
| ``soc <soctype>``  | soctype can be one of imx35, imx51, imx53, imx6, imx7, vf610,|
|                    |                             imx8mq                           |
+--------------------+--------------------------------------------------------------+
| ``loadaddr <adr>`` |     The address the binary is uploaded to                    |
+--------------------+--------------------------------------------------------------+
| ``ivtofs <ofs>``   | The offset of the image header in the image. This should be: |
|                    |                                                              |
|                    | * ``0x400``:  MMC/SD, NAND, serial ROM, PATA, SATA           |
|                    | * ``0x1000``: NOR Flash                                      |
|                    | * ``0x100``: OneNAND                                         |
+--------------------+--------------------------------------------------------------+

Memory manipulation:

+----------------------------------------+-------------------------------------------------+
| ``wm 8 <addr> <value>``                | write ``<value>`` into byte ``<addr>``          |
+----------------------------------------+-------------------------------------------------+
| ``wm 16 <addr> <value>``               | write ``<value>`` into short ``<addr>``         |
+----------------------------------------+-------------------------------------------------+
| ``wm 32 <addr> <value>``               | write ``<value>`` into word ``<addr>``          |
+----------------------------------------+-------------------------------------------------+
| ``set_bits <width> <addr> <value>``    | set set bits in ``<value>`` in ``<addr>``       |
+----------------------------------------+-------------------------------------------------+
| ``clear_bits <width> <addr> <value>``  | clear set bits in ``<value>`` in ``<addr>``     |
+----------------------------------------+-------------------------------------------------+
| ``nop``                                | do nothing (just waste time)                    |
+----------------------------------------+-------------------------------------------------+

``<width>`` can be one of 8, 16 or 32.

Checking conditions:

+----------------------------------------+-----------------------------------------+
| ``check <width> <cond> <addr> <mask>`` | Poll until condition becomes true.      |
|                                        | with ``<cond>`` being one of:           |
|                                        |                                         |
|                                        | * ``until_all_bits_clear``              |
|                                        | * ``until_all_bits_set``                |
|                                        | * ``until_any_bit_clear``               |
|                                        | * ``until_any_bit_set``                 |
+----------------------------------------+-----------------------------------------+

Some notes about the mentioned *conditions*.

 - ``until_all_bits_clear`` waits until ``(*addr & mask) == 0`` is true
 - ``until_all_bits_set`` waits until ``(*addr & mask) == mask`` is true
 - ``until_any_bit_clear`` waits until ``(*addr & mask) != mask`` is true
 - ``until_any_bit_set`` waits until ``(*addr & mask) != 0`` is true.

USB Boot
^^^^^^^^

Most boards can be explicitly configured for USB Boot Mode or fall back
to USB Boot when no other medium can be found. The barebox repository
contains a USB upload tool. As it depends on the libusb development headers,
it is not built by default. Enable it explicitly in ``make menuconfig``
and install the libusb development package. On Debian, this can be done
with ``apt-get install libusb-dev``. After compilation, the tool can be used
with only the image name as argument:

.. code-block:: sh

  scripts/imx/imx-usb-loader images/barebox-freescale-imx51-babbage.img

External Boot Mode
------------------

The External Boot Mode is supported by the older i.MX SoCs:

* i.MX1
* i.MX21
* i.MX27
* i.MX31
* i.MX35

The External Boot Mode supports booting only from NOR and NAND flash. On NOR
flash, the binary is started directly on its physical address in memory. Booting
from NAND flash is more complicated. The NAND flash controller copies the first
2kb of the image to the NAND Controller's internal SRAM. This initial binary
portion then has to:

* Set up the SDRAM
* Copy the initial binary to SDRAM to make the internal SRAM in the NAND flash
  controller free for use for the controller
* Copy the whole barebox image to SDRAM
* Start the image

It is possible to write the image directly to NAND. However, since NAND flash
can have bad blocks which must be skipped during writing the image and also
by the initial loader, it is recommended to use the :ref:`command_barebox_update`
command for writing to NAND flash.

i.MX boards
-----------

Not all supported boards have a description here. Many newer boards also do
not have individual defconfig files, they are covered by ``imx_v7_defconfig``
or ``imx_defconfig`` instead.

.. toctree::
  :glob:
  :maxdepth: 2

  imx/*
  imx/*/*
