package main

import (
	"bufio"
	"fmt"
	"io"
	"strings"
)

type setupOptions struct {
	URL, Directory  string
	ChooseDirectory bool
	Start           bool
}

// Keep startup interaction separate from Windows dialogs and receiver transport.
// Explicit command-line options skip the menu; double-click launches can edit
// remembered settings without deleting JSON or knowing command-line flags.
func setupReceiver(config receiverConfig, options setupOptions, input io.Reader, output io.Writer,
	chooseDirectory func() (string, error)) (receiverConfig, error) {
	reader := bufio.NewReader(input)
	config.Token = ""
	if options.URL != "" {
		if err := config.setURL(options.URL); err != nil {
			return config, err
		}
	}
	if options.Directory != "" {
		config.Directory = options.Directory
	}
	editAddress, editDirectory := config.URL == "", config.Directory == "" || (options.ChooseDirectory && options.Directory == "")
	interactive := !options.Start && options.URL == "" && options.Directory == "" && !options.ChooseDirectory
	if interactive && config.URL != "" && config.Directory != "" {
		fmt.Fprintf(output, "当前设备：%s\n保存目录：%s\n", config.URL, config.Directory)
		for {
			fmt.Fprint(output, "\n回车 开始接收\n1 修改设备地址\n2 更换保存目录\n3 修改地址和目录\n0 退出\n请选择：")
			choice, err := readSetupLine(reader)
			if err != nil {
				return config, err
			}
			switch choice {
			case "":
			case "1":
				editAddress = true
			case "2":
				editDirectory = true
			case "3":
				editAddress, editDirectory = true, true
			case "0":
				return config, errUserStopped
			default:
				fmt.Fprintln(output, "请输入 1、2、3、0，或直接按回车。")
				continue
			}
			break
		}
	}
	if editAddress {
		for {
			fmt.Fprint(output, "设备地址（如 192.168.2.41:8080，HTTPS 请填写完整地址）")
			if config.URL != "" {
				fmt.Fprintf(output, " [%s，回车沿用]", config.URL)
			}
			fmt.Fprint(output, "：")
			address, err := readSetupLine(reader)
			if err != nil {
				return config, err
			}
			if address == "" {
				address = config.URL
			}
			address, err = normalizeReceiverURL(address)
			if err != nil {
				fmt.Fprintln(output, err)
				continue
			}
			// A deliberate menu edit can keep the selected root: the receiver
			// already separates images and progress by normalized device URL.
			config.URL = address
			break
		}
	}
	address, err := normalizeReceiverURL(config.URL)
	if err != nil {
		return config, err
	}
	config.URL = address
	if editDirectory {
		directory, err := chooseDirectory()
		if err != nil {
			fmt.Fprintln(output, err)
			fmt.Fprint(output, "请输入电脑图片保存目录（回车取消）：")
			directory, err = readSetupLine(reader)
			if err != nil {
				return config, err
			}
			directory = strings.Trim(directory, "\"")
		}
		if directory == "" {
			return config, errUserStopped
		}
		config.Directory = directory
	}
	return config, nil
}

func readSetupLine(reader *bufio.Reader) (string, error) {
	line, err := reader.ReadString('\n')
	if err != nil && !(err == io.EOF && line != "") {
		return "", err
	}
	return strings.TrimSpace(line), nil
}
