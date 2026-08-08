#pragma once
#import <Foundation/Foundation.h>

#include <string>

// std::string <-> NSString bridge used everywhere.
static inline NSString *N(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()]; }
static inline std::string S(NSString *s) { return s ? std::string(s.UTF8String) : std::string(); }
