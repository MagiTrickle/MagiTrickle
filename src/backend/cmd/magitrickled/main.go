package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"path"
	"strconv"
	"sync"
	"syscall"

	"magitrickle"
	"magitrickle/constant"

	"github.com/rs/zerolog"
	"github.com/rs/zerolog/log"
)

func getPIDPath(pid int) (string, error) {
	return os.Readlink(fmt.Sprintf("/proc/%d/exe", pid))
}

func checkPIDFile() (int, error) {
	data, err := os.ReadFile(constant.PIDPath)
	if err != nil {
		if os.IsNotExist(err) {
			return 0, nil
		}
		return 0, err
	}

	pid, err := strconv.Atoi(string(data))
	if err != nil {
		return 0, nil
	}

	currPID, _ := getPIDPath(os.Getpid())
	filePID, _ := getPIDPath(pid)
	if path.Base(currPID) == path.Base(filePID) {
		return pid, nil
	}

	_ = os.Remove(constant.PIDPath)
	return 0, nil
}

func createPIDFile() error {
	pid := os.Getpid()
	return os.WriteFile(constant.PIDPath, []byte(strconv.Itoa(pid)), 0644)
}

func removePIDFile() {
	_ = os.Remove(constant.PIDPath)
}

func main() {
	// Настройка zerolog
	consoleLogger := zerolog.ConsoleWriter{Out: os.Stderr}

	log.Logger = log.Output(consoleLogger)
	log.Info().
		Str("version", constant.Version).
		Msg("starting MagiTrickle daemon")

	pid, err := checkPIDFile()
	if err != nil {
		log.Fatal().Err(err).Msg("failed to check PID file")
	}
	if pid != 0 {
		log.Fatal().Msg(fmt.Sprintf("process %d is already running", pid))
	}

	if err := createPIDFile(); err != nil {
		log.Fatal().Err(err).Msg("failed to create PID file")
	}
	defer removePIDFile()

	app := magitrickle.New()

	log.Info().Msg("starting service")

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Запуск ядра приложения в горутине
	appResult := make(chan error, 1)
	go func() {
		appResult <- app.Start(ctx)
	}()

	// Обработка системных сигналов для graceful shutdown
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM, syscall.SIGHUP)

	var once sync.Once
	shutdown := func() {
		log.Info().Msg("shutting down service")
		cancel()
	}

	for {
		select {
		case err := <-appResult:
			if err != nil {
				log.Error().Err(err).Msg("failed to start application")
			}
			once.Do(shutdown)
			log.Info().Msg("service stopped")
			return
		case sig := <-sigChan:
			log.Info().Msgf("received signal: %v", sig)
			switch sig {
			case os.Interrupt, syscall.SIGTERM:
				once.Do(shutdown)
			case syscall.SIGHUP:
				if err := app.LoadConfig(); err != nil {
					log.Error().Err(err).Msg("failed to reload config")
				}
			}
		}
	}
}
