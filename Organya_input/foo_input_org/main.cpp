#include "stdafx.h"

DECLARE_COMPONENT_VERSION(
	"Organya Decoder",
	"1.0",
	"Organya (.org) music file decoder for foobar2000.\n\n"
	"Organya is a sequenced music format created by Studio Pixel,\n"
	"used in Cave Story and other games.\n\n"
	"Organya 3.0.7\n"
	"Copyright (c) 1999 Studio Pixel\n"
	"Org Maker 2 by Rxo Inverse\n"
	"Org Maker 3 by Strultz\n"
	"foo_input_org by Christopher Snowhill"
);

VALIDATE_COMPONENT_FILENAME("foo_input_org.dll");

FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
