package app

import (
	"encoding/json"
	"os"
	"sync"

	"github.com/rs/zerolog"
	"github.com/rs/zerolog/log"
)

// LogEvent описывает структуру лог-события для API-подписок
type LogEvent struct {
	Raw json.RawMessage `json:"raw"`
}

// LogSubscription представляет подписку на поток логов
type LogSubscription struct {
	ch chan LogEvent
}

// Out возвращает канал для чтения логов
func (s *LogSubscription) Out() <-chan LogEvent { return s.ch }

// Close закрывает канал подписки
func (s *LogSubscription) Close() { close(s.ch) }

// loggingHub хранит активные подписки
type loggingHub struct {
	mu          sync.RWMutex
	subscribers map[*LogSubscription]struct{}
}

var globalHub = &loggingHub{
	subscribers: make(map[*LogSubscription]struct{}),
}

// SubscribeLogs создаёт новую подписку с небуферизованным каналом
func SubscribeLogs() *LogSubscription {
	sub := &LogSubscription{ch: make(chan LogEvent)}
	globalHub.mu.Lock()
	globalHub.subscribers[sub] = struct{}{}
	globalHub.mu.Unlock()
	return sub
}

// UnsubscribeLogs удаляет подписку и закрывает её канал
func UnsubscribeLogs(sub *LogSubscription) {
	globalHub.mu.Lock()
	delete(globalHub.subscribers, sub)
	globalHub.mu.Unlock()
	sub.Close()
}

// notifySubscribers рассылает лог-событие всем активным подпискам
func notifySubscribers(ev LogEvent) {
	globalHub.mu.RLock()
	defer globalHub.mu.RUnlock()
	for sub := range globalHub.subscribers {
		select {
		case sub.ch <- ev:
		default:
			// событие пропускается
		}
	}
}

// splitWriter – собственный writer, который перехватывает исходное JSON-сообщение
type splitWriter struct {
	console zerolog.ConsoleWriter
}

func (sw *splitWriter) Write(p []byte) (n int, err error) {
	// Отправляем подписчикам исходное JSON-сообщение
	notifySubscribers(LogEvent{Raw: json.RawMessage(p)})
	return sw.console.Write(p)
}

// SelectLogLevel выбирает уровень логирования по строке и устанавливает его глобально
func SelectLogLevel(logLevel string) {
	lvl, err := zerolog.ParseLevel(logLevel)
	if err != nil {
		lvl = zerolog.InfoLevel
	}
	zerolog.SetGlobalLevel(lvl)
}

// StartLogs настраивает глобальный логгер
func StartLogs() {
	consoleWriter := zerolog.ConsoleWriter{Out: os.Stdout}
	sw := &splitWriter{console: consoleWriter}

	logger := zerolog.New(sw).With().Timestamp().Logger()
	log.Logger = logger
}
