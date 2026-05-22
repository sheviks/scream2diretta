#!/bin/bash
#
# scream2diretta - Installation Script
#
# Run with: bash scripts/install.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_BIN="/usr/local/bin"
SERVICE_FILE="/etc/systemd/system/scream2diretta.service"
CONFIG_FILE="/etc/default/scream2diretta"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

print_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_error()   { echo -e "${RED}[ERROR]${NC} $1"; }
print_header()  { echo -e "\n${CYAN}=== $1 ===${NC}\n"; }

# Run command with root privileges.
# If already root, run directly.
# If not root, try sudo. If sudo is not available, error out.
run_privileged() {
    if [ "$EUID" -eq 0 ]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        print_error "Root privileges required but sudo is not available."
        print_info "Please run this script as root or install sudo."
        exit 1
    fi
}

confirm() {
    local prompt="$1"
    local default="${2:-N}"
    local response
    if [[ "$default" =~ ^[Yy]$ ]]; then
        read -p "$prompt [Y/n]: " response
        response=${response:-Y}
    else
        read -p "$prompt [y/N]: " response
        response=${response:-N}
    fi
    [[ "$response" =~ ^[Yy]$ ]]
}

detect_arch_name() {
    local arch=$(uname -m)
    case "$arch" in
        x86_64)
            if grep -q 'avx512f' /proc/cpuinfo 2>/dev/null; then
                echo "x64-linux-15v4"
            elif grep -q 'avx2' /proc/cpuinfo 2>/dev/null; then
                echo "x64-linux-15v3"
            else
                echo "x64-linux-15v3"
            fi
            ;;
        aarch64)
            local page_size=$(getconf PAGE_SIZE 2>/dev/null || echo 4096)
            if [ "$page_size" = "16384" ]; then
                echo "aarch64-linux-15k16"
            else
                echo "aarch64-linux-15"
            fi
            ;;
        *)
            echo ""
            ;;
    esac
}

detect_system() {
    print_header "System Detection"
    if [ "$EUID" -eq 0 ]; then
        print_warning "Running as root (sudo is not required)"
    elif command -v sudo >/dev/null 2>&1; then
        print_info "Running as non-root user; sudo will be used for privileged operations"
    else
        print_error "This script requires root privileges, but sudo is not available."
        print_info "Please run as root, install sudo, or use a system that supports it (e.g., GentooPlayer should be run as root)."
        exit 1
    fi
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        OS=$ID
        VER=$VERSION_ID
        print_success "Detected: $PRETTY_NAME"
    else
        print_error "Cannot detect Linux distribution"
        exit 1
    fi
    ARCH=$(uname -m)
    print_info "Architecture: $ARCH"
    ARCH_NAME=$(detect_arch_name)
    if [ -n "$ARCH_NAME" ]; then
        print_info "Auto-detected ARCH_NAME: $ARCH_NAME"
    else
        print_warning "Could not auto-detect ARCH_NAME for $ARCH"
    fi
}

detect_latest_sdk() {
    # Search from known locations: project root, project parent, $HOME, /opt, /usr/local
    local sdk_found=$(find "$SCRIPT_DIR" "$SCRIPT_DIR/.." "$HOME" /opt /usr/local \
        -maxdepth 1 -type d -name 'DirettaHostSDK_*' 2>/dev/null | sort -V | tail -1 | xargs realpath 2>/dev/null)
    if [ -n "$sdk_found" ]; then
        echo "$sdk_found"
    else
        echo "$HOME/DirettaHostSDK"
    fi
}

SDK_PATH="${DIRETTA_SDK_PATH:-$(detect_latest_sdk)}"

install_dependencies() {
    print_header "Installing Build Dependencies"
    case $OS in
        fedora|rhel|centos)
            run_privileged dnf install -y gcc-c++ make cmake pkgconfig
            ;;
        ubuntu|debian|dietpi)
            run_privileged apt update
            run_privileged apt install -y build-essential cmake pkg-config
            ;;
        arch|archarm|manjaro)
            run_privileged pacman -Sy --needed --noconfirm base-devel cmake pkgconf
            ;;
        *)
            print_error "Unsupported distribution: $OS"
            print_info "Please install manually: gcc/g++ (C++17), cmake (>= 3.7), make, pkg-config"
            exit 1
            ;;
    esac
    print_success "Build dependencies installed"
}

check_diretta_sdk() {
    print_header "Diretta SDK Check"
    local sdk_candidates=()
    while IFS= read -r sdk_dir; do
        sdk_candidates+=("$sdk_dir")
    done < <(find "$SCRIPT_DIR" "$SCRIPT_DIR/.." "$HOME" /opt /usr/local \
        -maxdepth 1 -type d -name 'DirettaHostSDK_*' 2>/dev/null | sort -Vr)
    [ -d "$SDK_PATH" ] && sdk_candidates=("$SDK_PATH" "${sdk_candidates[@]}")
    for loc in "${sdk_candidates[@]}"; do
        if [ -d "$loc" ] && [ -d "$loc/lib" ]; then
            SDK_PATH="$loc"
            local sdk_version=$(basename "$loc" | sed 's/DirettaHostSDK_//')
            print_success "Found Diretta SDK at: $SDK_PATH"
            [ -n "$sdk_version" ] && print_info "SDK version: $sdk_version"
            return 0
        fi
    done
    print_warning "Diretta SDK not found"
    echo ""
    echo "The Diretta Host SDK is required but not redistributed."
    echo "Download from: https://www.diretta.link/hostsdk.html"
    echo "Extract to: $HOME/"
    echo ""
    read -p "Press Enter after you've extracted the SDK..."
    while IFS= read -r sdk_dir; do
        if [ -d "$sdk_dir" ] && [ -d "$sdk_dir/lib" ]; then
            SDK_PATH="$sdk_dir"
            print_success "Found Diretta SDK at: $SDK_PATH"
            return 0
        fi
    done < <(find "$SCRIPT_DIR" "$SCRIPT_DIR/.." "$HOME" /opt -maxdepth 1 -type d -name 'DirettaHostSDK_*' 2>/dev/null | sort -Vr)
    print_error "SDK still not found. Please extract it and try again."
    exit 1
}

build_scream2diretta() {
    print_header "Building scream2diretta"
    cd "$SCRIPT_DIR"
    if [ -d "build" ]; then
        print_info "Cleaning previous build..."
        rm -rf build
    fi
    mkdir -p build
    export DIRETTA_SDK_PATH="$(realpath "$SDK_PATH")"
    cd build
    print_info "Configuring with CMake..."
    local cmake_args="-DDIRETTA_ENABLE=ON -DDIRETTA_SDK_ROOT=$DIRETTA_SDK_PATH"
    if [ -n "$ARCH_NAME" ]; then
        print_info "Using DIRETTA_ARCH_SUFFIX=$ARCH_NAME"
        cmake_args="$cmake_args -DDIRETTA_ARCH_SUFFIX=$ARCH_NAME"
    fi
    cmake $cmake_args ..
    print_info "Building..."
    make -j$(nproc)
    if [ -f "scream2diretta" ]; then
        print_success "Build successful!"
        print_info "Binary: $SCRIPT_DIR/build/scream2diretta"
    else
        print_error "Build failed. Check error messages above."
        exit 1
    fi
    cd "$SCRIPT_DIR"
}

setup_systemd_service() {
    print_header "Systemd Service Installation"
    local BINARY_PATH="$SCRIPT_DIR/build/scream2diretta"
    if [ ! -f "$BINARY_PATH" ]; then
        print_error "Binary not found at: $BINARY_PATH"
        print_info "Please build first (option 2)"
        return 1
    fi
    if ! confirm "Install scream2diretta as system service?" "Y"; then
        print_info "Skipping systemd service setup"
        return 0
    fi
    print_info "Installing binary..."
    run_privileged cp "$BINARY_PATH" "$INSTALL_BIN/scream2diretta"
    run_privileged chmod +x "$INSTALL_BIN/scream2diretta"
    print_success "Binary installed: $INSTALL_BIN/scream2diretta"
    print_info "Installing systemd service..."
    run_privileged cp "$SCRIPT_DIR/scripts/scream2diretta.service" "$SERVICE_FILE"
    print_success "Service file installed: $SERVICE_FILE"
    print_info "Installing configuration file..."
    if [ ! -f "$CONFIG_FILE" ]; then
        run_privileged cp "$SCRIPT_DIR/scripts/scream2diretta.default" "$CONFIG_FILE"
        print_success "Configuration file installed: $CONFIG_FILE"
    else
        if ! diff -q "$SCRIPT_DIR/scripts/scream2diretta.default" "$CONFIG_FILE" > /dev/null 2>&1; then
            print_warning "Configuration file has changed in this version"
            if confirm "Update configuration file? (old file backed up as .bak)"; then
                run_privileged cp "$CONFIG_FILE" "${CONFIG_FILE}.bak"
                run_privileged cp "$SCRIPT_DIR/scripts/scream2diretta.default" "$CONFIG_FILE"
                print_success "Configuration updated (backup: ${CONFIG_FILE}.bak)"
            else
                print_info "Keeping current configuration"
            fi
        else
            print_info "Configuration file is up to date"
        fi
    fi
    print_info "Reloading systemd daemon..."
    run_privileged systemctl daemon-reload
    echo ""
    print_info "Listing available Diretta targets..."
    run_privileged "$INSTALL_BIN/scream2diretta" --list-targets 2>&1 || true
    echo ""
    read -p "Enter Diretta target number to enable (e.g., 1) or press Enter to skip: " TARGET_NUM
    if [ -n "$TARGET_NUM" ]; then
        if [ -f "$CONFIG_FILE" ]; then
            run_privileged sed -i "s/^TARGET=.*/TARGET=${TARGET_NUM}/" "$CONFIG_FILE"
        fi
        run_privileged systemctl enable scream2diretta.service
        print_success "Service enabled with target $TARGET_NUM (starts on boot)"
    fi
    echo ""
    print_success "Installation complete!"
    echo ""
    echo "  Binary:        $INSTALL_BIN/scream2diretta"
    echo "  Configuration: $CONFIG_FILE"
    echo ""
    echo "  Next steps:"
    echo "    1. Edit configuration (optional):"
    echo "       sudo nano $CONFIG_FILE"
    echo ""
    echo "    2. Start the service:"
    echo "       sudo systemctl start scream2diretta"
    echo ""
    echo "    3. Check status:"
    echo "       sudo systemctl status scream2diretta"
    echo ""
    echo "    4. View logs:"
    echo "       sudo journalctl -u scream2diretta -f"
    echo ""
}

update_binary() {
    print_header "Updating scream2diretta"
    local BINARY_PATH="$SCRIPT_DIR/build/scream2diretta"
    if [ ! -f "$BINARY_PATH" ]; then
        print_error "Binary not found. Please build first."
        return 1
    fi
    if [ ! -f "$INSTALL_BIN/scream2diretta" ]; then
        print_error "scream2diretta not installed. Use 'Install service' first."
        return 1
    fi
    if systemctl is-active --quiet scream2diretta.service 2>/dev/null; then
        print_info "Stopping service..."
        run_privileged systemctl stop scream2diretta.service
    fi
    run_privileged cp "$BINARY_PATH" "$INSTALL_BIN/scream2diretta"
    run_privileged chmod +x "$INSTALL_BIN/scream2diretta"
    if [ -f "$SERVICE_FILE" ]; then
        run_privileged cp "$SCRIPT_DIR/scripts/scream2diretta.service" "$SERVICE_FILE"
        run_privileged systemctl daemon-reload
    fi
    print_success "Binary updated: $INSTALL_BIN/scream2diretta"
    if systemctl is-active --quiet scream2diretta.service 2>/dev/null; then
        print_info "Restarting service..."
        run_privileged systemctl start scream2diretta.service
    fi
    print_success "Update complete!"
}

test_installation() {
    print_header "Testing Installation"
    local BINARY="$INSTALL_BIN/scream2diretta"
    if [ -f "$BINARY" ]; then
        print_success "Binary: $BINARY OK"
    elif [ -f "$SCRIPT_DIR/build/scream2diretta" ]; then
        BINARY="$SCRIPT_DIR/build/scream2diretta"
        print_success "Binary (build): $BINARY OK"
    else
        print_error "Binary: NOT FOUND"
        return 1
    fi
    if [ -f "$SERVICE_FILE" ]; then
        print_success "Systemd service: $SERVICE_FILE OK"
    else
        print_warning "Systemd service not installed"
    fi
    if [ -f "$CONFIG_FILE" ]; then
        print_success "Configuration: $CONFIG_FILE OK"
    else
        print_warning "Configuration not installed"
    fi
    echo ""
    print_info "Searching for Diretta targets..."
    run_privileged timeout 10 "$BINARY" --list-targets 2>&1 || {
        local exit_code=$?
        if [ $exit_code -eq 124 ]; then
            print_info "Target search timed out (normal if no targets found)"
        else
            print_warning "Could not list Diretta targets"
            print_info "Make sure a Diretta device is on your network"
        fi
    }
    echo ""
    if systemctl is-active scream2diretta.service &>/dev/null; then
        print_success "Service scream2diretta is running"
    else
        print_info "Service scream2diretta is not running"
    fi
    echo ""
    print_success "Test complete!"
}

uninstall() {
    print_header "Uninstall scream2diretta"
    echo "This will remove:"
    echo "  - Binary: $INSTALL_BIN/scream2diretta"
    echo "  - Service: $SERVICE_FILE"
    echo "  - Configuration: $CONFIG_FILE (optional)"
    echo ""
    if ! confirm "Proceed with uninstall?" "N"; then
        print_info "Uninstall cancelled"
        return 0
    fi
    run_privileged systemctl stop scream2diretta.service 2>/dev/null || true
    run_privileged systemctl disable scream2diretta.service 2>/dev/null || true
    print_info "Stopped and disabled service"
    if [ -f "$INSTALL_BIN/scream2diretta" ]; then
        run_privileged rm "$INSTALL_BIN/scream2diretta"
        print_success "Binary removed"
    fi
    if [ -f "$SERVICE_FILE" ]; then
        run_privileged rm "$SERVICE_FILE"
        run_privileged systemctl daemon-reload
        print_success "Service file removed"
    fi
    if [ -f "$CONFIG_FILE" ]; then
        if confirm "Remove configuration file ($CONFIG_FILE)?"; then
            run_privileged rm "$CONFIG_FILE"
            print_success "Configuration removed"
        else
            print_info "Configuration kept: $CONFIG_FILE"
        fi
    fi
    print_success "Uninstall complete"
}

show_main_menu() {
    echo ""
    echo "============================================"
    echo " scream2diretta - Installation"
    echo "============================================"
    echo ""
    echo "  1) Full installation (recommended)"
    echo "     - Dependencies, build, systemd service"
    echo ""
    echo "  2) Build scream2diretta only"
    echo "     - Compile (assumes dependencies installed)"
    echo ""
    echo "  3) Install systemd service only"
    echo "     - Install as system service (assumes built)"
    echo ""
    echo "  4) Update binary only"
    echo "     - Replace installed binary after rebuild"
    echo ""
    echo "  5) Test installation"
    echo "     - Verify binaries and list Diretta targets"
    echo ""
    echo "  u) Uninstall"
    echo "  q) Quit"
    echo ""
}

run_full_installation() {
    install_dependencies
    check_diretta_sdk
    build_scream2diretta
    setup_systemd_service
    test_installation
    print_header "Installation Complete!"
    echo ""
    echo "  Quick start:"
    echo "    sudo systemctl start scream2diretta"
    echo "    sudo systemctl status scream2diretta"
    echo "    sudo journalctl -u scream2diretta -f"
    echo ""
}

main() {
    detect_system
    case "${1:-}" in
        --full|-f)
            run_full_installation
            exit 0
            ;;
        --build|-b)
            check_diretta_sdk
            build_scream2diretta
            exit 0
            ;;
        --service|-s)
            setup_systemd_service
            exit 0
            ;;
        --update|-u)
            check_diretta_sdk
            build_scream2diretta
            update_binary
            exit 0
            ;;
        --test|-t)
            test_installation
            exit 0
            ;;
        --uninstall)
            uninstall
            exit 0
            ;;
        --help|-h)
            echo "Usage: $0 [OPTION]"
            echo ""
            echo "Options:"
            echo "  --full, -f      Full installation"
            echo "  --build, -b     Build only"
            echo "  --service, -s   Install systemd service only"
            echo "  --update, -u    Rebuild and update installed binary"
            echo "  --test, -t      Test installation"
            echo "  --uninstall     Remove scream2diretta"
            echo "  --help, -h      Show this help"
            echo ""
            echo "Without options, shows interactive menu."
            exit 0
            ;;
    esac
    while true; do
        show_main_menu
        read -p "Choose option [1-5/u/q]: " choice
        case $choice in
            1)
                run_full_installation
                break
                ;;
            2)
                check_diretta_sdk
                build_scream2diretta
                ;;
            3)
                setup_systemd_service
                ;;
            4)
                update_binary
                ;;
            5)
                test_installation
                ;;
            u|U)
                uninstall
                ;;
            q|Q)
                print_info "Exiting..."
                exit 0
                ;;
            *)
                print_error "Invalid option: $choice"
                ;;
        esac
    done
}

main "$@"
