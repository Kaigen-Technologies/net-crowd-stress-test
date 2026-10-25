// generated player names: "<Adjective><Animal><0-999>" from a seed, the same on every platform (integers only)
#pragma once

#include "lib/typedefs.h"
#include "lib/string.h"
#include "lib/fmt.h"

#define NC_NAME_MAX 32
#define NC_NAME_WORDS 64

hz_global const char *const nc_name_adjectives[NC_NAME_WORDS] = {
    "Brave",  "Calm",   "Clever", "Cosmic", "Crisp",  "Daring", "Eager",  "Fancy",  "Fast",   "Fuzzy",  "Gentle",
    "Giant",  "Glad",   "Golden", "Grand",  "Happy",  "Hidden", "Humble", "Jolly",  "Keen",   "Kind",   "Lively",
    "Lucky",  "Mellow", "Mighty", "Misty",  "Modest", "Noble",  "Odd",    "Plucky", "Polite", "Proud",  "Quick",
    "Quiet",  "Rapid",  "Rosy",   "Royal",  "Rusty",  "Shiny",  "Silent", "Silver", "Sleepy", "Sly",    "Smooth",
    "Snappy", "Solar",  "Sunny",  "Swift",  "Tidy",   "Tiny",   "Turbo",  "Vivid",  "Warm",   "Wild",   "Windy",
    "Wise",   "Witty",  "Zany",   "Zesty",  "Amber",  "Azure",  "Breezy", "Cheery", "Dapper"};

hz_global const char *const nc_name_animals[NC_NAME_WORDS] = {
    "Alpaca",  "Badger",  "Beaver",  "Bison",   "Bobcat",  "Camel",   "Cheetah", "Cobra",   "Condor",  "Corgi",
    "Coyote",  "Crane",   "Dingo",   "Dolphin", "Eagle",   "Falcon",  "Ferret",  "Finch",   "Fox",     "Gecko",
    "Gibbon",  "Gopher",  "Heron",   "Hippo",   "Ibis",    "Iguana",  "Jackal",  "Jaguar",  "Koala",   "Lemur",
    "Lynx",    "Mamba",   "Marmot",  "Moose",   "Narwhal", "Newt",    "Ocelot",  "Orca",    "Osprey",  "Otter",
    "Panda",   "Panther", "Parrot",  "Pelican", "Penguin", "Puffin",  "Puma",    "Quokka",  "Rabbit",  "Raven",
    "Robin",   "Salmon",  "Seal",    "Shark",   "Sloth",   "Sparrow", "Tapir",   "Tiger",   "Toucan",  "Turtle",
    "Walrus",  "Wombat",  "Yak",     "Zebra"};

// the name, NUL-terminated (NC_NAME_MAX holds every one)
force_inline void nc_name(u32 seed, char *out, u32 cap) {
  cstr_format(out, cap, "%%%", fmt_cstr(nc_name_adjectives[seed % NC_NAME_WORDS]),
              fmt_cstr(nc_name_animals[(seed >> 6) % NC_NAME_WORDS]), fmt_u32((seed >> 12) % 1000));
}
