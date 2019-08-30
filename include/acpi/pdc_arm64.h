
/* _PDC bit definition for ARMv7 processors */

#ifndef __PDC_ARM_H__
#define __PDC_ARM_H__

/* BOZO: is this even necessary? */

/* _PDC bit definition for ARM processors */

#define ACPI_PDC_P_FFH			(0x0001)
#define ACPI_PDC_C_C1_HALT		(0x0002)
#define ACPI_PDC_T_FFH			(0x0004)
#define ACPI_PDC_SMP_C1PT		(0x0008)
#define ACPI_PDC_SMP_C2C3		(0x0010)
#define ACPI_PDC_SMP_P_SWCOORD		(0x0020)
#define ACPI_PDC_SMP_C_SWCOORD		(0x0040)
#define ACPI_PDC_SMP_T_SWCOORD		(0x0080)
#define ACPI_PDC_C_C1_FFH		(0x0100)
#define ACPI_PDC_C_C2C3_FFH		(0x0200)
#define ACPI_PDC_SMP_P_HWCOORD		(0x0800)

#define ACPI_PDC_EST_CAPABILITY_SMP	(ACPI_PDC_SMP_C1PT | \
					 ACPI_PDC_C_C1_HALT | \
					 ACPI_PDC_P_FFH)

#define ACPI_PDC_EST_CAPABILITY_SWSMP	(ACPI_PDC_SMP_C1PT | \
					 ACPI_PDC_C_C1_HALT | \
					 ACPI_PDC_SMP_P_SWCOORD | \
					 ACPI_PDC_SMP_P_HWCOORD | \
					 ACPI_PDC_P_FFH)

#define ACPI_PDC_C_CAPABILITY_SMP	(ACPI_PDC_SMP_C2C3  | \
					 ACPI_PDC_SMP_C1PT  | \
					 ACPI_PDC_C_C1_HALT | \
					 ACPI_PDC_C_C1_FFH  | \
					 ACPI_PDC_C_C2C3_FFH)

#endif				/* __PDC_ARM_H__ */
