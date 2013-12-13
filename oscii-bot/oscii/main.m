//
//  main.m
//  oscii
//
//  Created by Justin-o on 12/12/13.
//  Copyright (c) 2013 cockos. All rights reserved.
//

#import <Cocoa/Cocoa.h>

int main(int argc, const char * argv[])
{
  extern char **g_argv;
  extern int g_argc;
  g_argc=argc;
  g_argv=argv;
  return NSApplicationMain(argc, argv);
}
