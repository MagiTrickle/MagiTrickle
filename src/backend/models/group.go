package models

import (
	"magitrickle/utils/intID"
)

type Group struct {
	ID        intID.ID
	Name      string
	Color     string
	Interface string
	Enable    bool
	Rules     []*Rule
}
