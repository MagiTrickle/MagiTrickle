package models

import (
	"github.com/Ponywka/MagiTrickle/backend/pkg/api/types"
)

type Group struct {
	ID        types.ID
	Name      string
	Color     string
	Interface string
	Enable    bool
	Rules     []*Rule
}
