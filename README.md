# smileOS
an Operating System Made For Beginners
# smileOS

smileOS is a lightweight experimental operating system built for learning, performance, and simplicity.

The project focuses on creating a minimal desktop operating system from the ground up while gradually adding core functionality such as graphical interfaces, storage support, system applications, and hardware interaction.

## Features

- Custom boot process
- Graphical desktop environment
- Mouse support
- Keyboard support
- Terminal application
- Settings application
- Notepad application
- Lightweight architecture
- Fast boot times
- FAT32 storage support *(in progress)*

## Architecture

smileOS is currently built with low-level system components including:

- Kernel initialization
- Memory management
- Basic drivers
- Input handling
- Graphical rendering
- Application framework
- Filesystem integration

## Roadmap

Planned improvements include:

- Package manager
- Network support
- Improved filesystem compatibility
- Additional built-in applications
- Performance optimizations
- UI improvements
- Security enhancements

## Building

Clone the repository:

```bash
git clone https://github.com/studi8audioeimagem-lgtm/smileOS.git
cd smileOS
```

Build the project:

```bash
make
```

## Running

Run using QEMU:

```bash
qemu-system-x86_64 -cdrom os.iso -m 512
```

Or use:

```bash
make run
```

## Project Goals

The goal of smileOS is to explore operating system development while maintaining a clean, lightweight, and user-friendly design.

This project serves as both a learning platform and an experimental operating system.

## Current Status

smileOS is under active development and should be considered experimental.

## Contributing

Contributions, suggestions, and feedback are welcome.

## License

This project is licensed under the MIT License.
