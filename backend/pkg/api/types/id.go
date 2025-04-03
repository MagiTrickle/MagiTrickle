package types

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
)

var ErrIDWrongLength = errors.New("id wrong length")

type ID [4]byte

func (id ID) String() string {
	return hex.EncodeToString(id[:])
}

func (id ID) MarshalText() ([]byte, error) {
	return []byte(id.String()), nil
}

func (id *ID) UnmarshalText(data []byte) error {
	if len(data) != 8 {
		return ErrIDWrongLength
	}
	decoded, err := hex.DecodeString(string(data))
	if err != nil {
		return err
	}
	copy(id[:], decoded)
	return nil
}

func ParseID(idStr string) (ID, error) {
	var id ID
	err := id.UnmarshalText([]byte(idStr))
	return id, err
}

func RandomID() ID {
	var id ID
	_, err := rand.Read(id[:])
	if err != nil {
		panic(err)
	}
	return id
}
