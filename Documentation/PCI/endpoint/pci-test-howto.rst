.. SPDX-License-Identifier: GPL-2.0

===================
PCI Test User Guide
===================

:Author: Kishon Vijay Abraham I <kishon@ti.com>

This document is a guide to help users use pci-epf-test function driver
and pci_endpoint_test host driver for testing PCI. The list of steps to
be followed in the host side and EP side is given below.

Endpoint Device
===============

Endpoint Controller Devices
---------------------------

To find the list of endpoint controller devices in the system::

	# ls /sys/class/pci_epc/
	  51000000.pcie_ep

If PCI_ENDPOINT_CONFIGFS is enabled::

	# ls /sys/kernel/config/pci_ep/controllers
	  51000000.pcie_ep


Endpoint Function Drivers
-------------------------

To find the list of endpoint function drivers in the system::

	# ls /sys/bus/pci-epf/drivers
	  pci_epf_test

If PCI_ENDPOINT_CONFIGFS is enabled::

	# ls /sys/kernel/config/pci_ep/functions
	  pci_epf_test


Creating pci-epf-test Device
----------------------------

PCI endpoint function device can be created using the configfs. To create
pci-epf-test device, the following commands can be used::

	# mount -t configfs none /sys/kernel/config
	# cd /sys/kernel/config/pci_ep/
	# mkdir functions/pci_epf_test/func1

The "mkdir func1" above creates the pci-epf-test function device that will
be probed by pci_epf_test driver.

The PCI endpoint framework populates the directory with the following
configurable fields::

	# ls functions/pci_epf_test/func1
	  baseclass_code	interrupt_pin	progif_code	subsys_id
	  cache_line_size	msi_interrupts	revid		subsys_vendorid
	  deviceid          	msix_interrupts	subclass_code	vendorid

The PCI endpoint function driver populates these entries with default values
when the device is bound to the driver. The pci-epf-test driver populates
vendorid with 0xffff and interrupt_pin with 0x0001::

	# cat functions/pci_epf_test/func1/vendorid
	  0xffff
	# cat functions/pci_epf_test/func1/interrupt_pin
	  0x0001


Configuring pci-epf-test Device
-------------------------------

The user can configure the pci-epf-test device using configfs entry. In order
to change the vendorid and the number of MSI interrupts used by the function
device, the following commands can be used::

	# echo 0x104c > functions/pci_epf_test/func1/vendorid
	# echo 0xb500 > functions/pci_epf_test/func1/deviceid
	# echo 16 > functions/pci_epf_test/func1/msi_interrupts
	# echo 8 > functions/pci_epf_test/func1/msix_interrupts


Binding pci-epf-test Device to EP Controller
--------------------------------------------

In order for the endpoint function device to be useful, it has to be bound to
a PCI endpoint controller driver. Use the configfs to bind the function
device to one of the controller driver present in the system::

	# ln -s functions/pci_epf_test/func1 controllers/51000000.pcie_ep/

Once the above step is completed, the PCI endpoint is ready to establish a link
with the host.


Start the Link
--------------

In order for the endpoint device to establish a link with the host, the _start_
field should be populated with '1'::

	# echo 1 > controllers/51000000.pcie_ep/start


RootComplex Device
==================

lspci Output
------------

Note that the devices listed here correspond to the value populated in 1.4
above::

	00:00.0 PCI bridge: Texas Instruments Device 8888 (rev 01)
	01:00.0 Unassigned class [ff00]: Texas Instruments Device b500


Using Endpoint Test function Device
-----------------------------------

pcitest.sh added in tools/pci/ can be used to run all the default PCI endpoint
tests. To compile this tool the following commands should be used::

	# cd <kernel-dir>
	# make -C tools/pci

or if you desire to compile and install in your system::

	# cd <kernel-dir>
	# make -C tools/pci install

The tool and script will be located in <rootfs>/usr/bin/


pcitest.sh Output
~~~~~~~~~~~~~~~~~
::

	# pcitest.sh
	BAR tests

	BAR0:           SUCCESS
	BAR1:           SUCCESS
	BAR2:           SUCCESS
	BAR3:           SUCCESS
	BAR4:           FAILED
	BAR5:           FAILED

	Interrupt tests

	SET IRQ TYPE TO LEGACY:         SUCCESS
	LEGACY IRQ:     FAILED
	SET IRQ TYPE TO MSI:            SUCCESS
	MSI1:           SUCCESS
	MSI2:           SUCCESS
	MSI3:           SUCCESS
	MSI4:           SUCCESS
	MSI5:           SUCCESS
	MSI6:           SUCCESS
	MSI7:           SUCCESS
	MSI8:           SUCCESS
	MSI9:           SUCCESS
	MSI10:          SUCCESS
	MSI11:          SUCCESS
	MSI12:          SUCCESS
	MSI13:          SUCCESS
	MSI14:          SUCCESS
	MSI15:          SUCCESS
	MSI16:          SUCCESS
	MSI17:          FAILED
	MSI18:          FAILED
	MSI19:          FAILED
	MSI20:          FAILED
	MSI21:          FAILED
	MSI22:          FAILED
	MSI23:          FAILED
	MSI24:          FAILED
	MSI25:          FAILED
	MSI26:          FAILED
	MSI27:          FAILED
	MSI28:          FAILED
	MSI29:          FAILED
	MSI30:          FAILED
	MSI31:          FAILED
	MSI32:          FAILED
	SET IRQ TYPE TO MSI-X:          SUCCESS
	MSI-X1:         SUCCESS
	MSI-X2:         SUCCESS
	MSI-X3:         SUCCESS
	MSI-X4:         SUCCESS
	MSI-X5:         SUCCESS
	MSI-X6:         SUCCESS
	MSI-X7:         SUCCESS
	MSI-X8:         SUCCESS
	MSI-X9:         FAILED
	MSI-X10:        FAILED
	MSI-X11:        FAILED
	MSI-X12:        FAILED
	MSI-X13:        FAILED
	MSI-X14:        FAILED
	MSI-X15:        FAILED
	MSI-X16:        FAILED
	[...]
	MSI-X2047:      FAILED
	MSI-X2048:      FAILED

	Read Tests

	SET IRQ TYPE TO MSI:            SUCCESS
	READ (      1 bytes):           SUCCESS
	READ (   1024 bytes):           SUCCESS
	READ (   1025 bytes):           SUCCESS
	READ (1024000 bytes):           SUCCESS
	READ (1024001 bytes):           SUCCESS

	Write Tests

	WRITE (      1 bytes):          SUCCESS
	WRITE (   1024 bytes):          SUCCESS
	WRITE (   1025 bytes):          SUCCESS
	WRITE (1024000 bytes):          SUCCESS
	WRITE (1024001 bytes):          SUCCESS

	Copy Tests

	COPY (      1 bytes):           SUCCESS
	COPY (   1024 bytes):           SUCCESS
	COPY (   1025 bytes):           SUCCESS
	COPY (1024000 bytes):           SUCCESS
	COPY (1024001 bytes):           SUCCESS
