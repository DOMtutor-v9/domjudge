<?php declare(strict_types=1);

namespace App\Entity;

use App\DataTransferObject\FileWithName;
use App\Utils\Utils;
use Doctrine\Common\Collections\ArrayCollection;
use Doctrine\Common\Collections\Collection;
use Doctrine\ORM\Mapping as ORM;
use Exception;
use JMS\Serializer\Annotation as Serializer;
use Symfony\Bridge\Doctrine\Validator\Constraints\UniqueEntity;
use Symfony\Component\HttpFoundation\File\UploadedFile;
use Symfony\Component\HttpFoundation\StreamedResponse;
use Symfony\Component\HttpKernel\Exception\BadRequestHttpException;
use Symfony\Component\Validator\Constraints as Assert;

/**
 * Stores testcases per problem.
 */
#[ORM\Entity]
#[ORM\Table(options: [
    'collation' => 'utf8mb4_unicode_ci',
    'charset' => 'utf8mb4',
    'comment' => 'Problems the teams can submit solutions for',
])]
#[ORM\UniqueConstraint(columns: ['externalid'], name: 'externalid', options: ['lengths' => [190]])]
#[ORM\Index(columns: ['special_run'], name: 'special_run')]
#[ORM\Index(columns: ['special_compare'], name: 'special_compare')]
#[ORM\HasLifecycleCallbacks]
#[UniqueEntity(fields: 'externalid')]
class Problem extends BaseApiEntity implements
    HasExternalIdInterface,
    ExternalIdFromInternalIdInterface,
    PrefixedExternalIdInterface
{
    #[ORM\Id]
    #[ORM\GeneratedValue]
    #[ORM\Column(options: ['comment' => 'Problem ID', 'unsigned' => true])]
    #[Serializer\Exclude]
    protected ?int $probid = null;

    #[ORM\Column(
        nullable: true,
        options: [
            'comment' => 'Problem ID in an external system, should be unique inside a single contest',
            'collation' => 'utf8mb4_bin',
        ]
    )]
    #[Serializer\SerializedName('id')]
    protected ?string $externalid = null;

    #[ORM\Column(options: ['comment' => 'Descriptive name'])]
    #[Assert\NotBlank]
    private string $name = 'Unknown name';

    #[ORM\Column(options: [
        'comment' => 'Maximum run time (in seconds) for this problem',
        'default' => 0,
        'unsigned' => true,
    ])]
    #[Assert\GreaterThan(0)]
    #[Serializer\Exclude]
    private float $timelimit = 0;

    #[ORM\Column(
        nullable: true,
        options: [
            'comment' => 'Maximum memory available (in kB) for this problem',
            'unsigned' => true,
        ]
    )]
    #[Assert\GreaterThan(0)]
    #[Serializer\Exclude]
    private ?int $memlimit = null;

    #[ORM\Column(
        nullable: true,
        options: ['comment' => 'Maximum output size (in kB) for this problem', 'unsigned' => true]
    )]
    #[Assert\GreaterThan(0)]
    #[Serializer\Exclude]
    private ?int $outputlimit = null;

    #[ORM\Column(nullable: true, options: ['comment' => 'Optional arguments to special_compare script'])]
    #[Serializer\Exclude]
    private ?string $special_compare_args = null;

    #[Assert\File]
    #[Serializer\Exclude]
    private ?UploadedFile $problemstatementFile = null;

    #[Serializer\Exclude]
    private bool $clearProblemstatement = false;

    #[ORM\Column(
        length: 4,
        nullable: true,
        options: ['comment' => 'File type of problem statement']
    )]
    #[Serializer\Exclude]
    private ?string $problemstatement_type = null;

    // These types are encoded as bitset - if you add a new type, use the next power of 2.
    public const TYPE_PASS_FAIL = 1;
    public const TYPE_SCORING = 2;
    public const TYPE_MULTI_PASS = 4;
    public const TYPE_INTERACTIVE = 8;
    public const TYPE_SUBMIT_ANSWER = 16;

    /**
     * @var array<int, string>
     */
    #[Serializer\Exclude]
    private array $typesToString = [
        self::TYPE_PASS_FAIL => 'pass-fail',
        self::TYPE_SCORING => 'scoring',
        self::TYPE_MULTI_PASS => 'multi-pass',
        self::TYPE_INTERACTIVE => 'interactive',
        self::TYPE_SUBMIT_ANSWER => 'submit-answer',
    ];

    #[ORM\Column(options: ['comment' => 'Bitmask of problem types, default is pass-fail.'])]
    #[Serializer\Exclude]
    private int $types = self::TYPE_PASS_FAIL;

    #[ORM\Column(
        nullable: true,
        options: ['comment' => 'Optional limit on the number of rounds; defaults to 1 for traditional problems, 2 for multi-pass problems if not specified.', 'unsigned' => true]
    )]
    #[Assert\GreaterThan(0)]
    #[Serializer\Exclude]
    private ?int $multipassLimit = null;

    /**
     * @var Collection<int, Submission>
     */
    #[ORM\OneToMany(mappedBy: 'problem', targetEntity: Submission::class)]
    #[Serializer\Exclude]
    private Collection $submissions;

    /**
     * @var Collection<int, Clarification>
     */
    #[ORM\OneToMany(mappedBy: 'problem', targetEntity: Clarification::class)]
    #[Serializer\Exclude]
    private Collection $clarifications;

    /**
     * @var Collection<int, ContestProblem>
     */
    #[ORM\OneToMany(mappedBy: 'problem', targetEntity: ContestProblem::class)]
    #[Serializer\Exclude]
    private Collection $contest_problems;

    #[ORM\ManyToOne(inversedBy: 'problems_compare')]
    #[ORM\JoinColumn(name: 'special_compare', referencedColumnName: 'execid', onDelete: 'SET NULL')]
    #[Serializer\Exclude]
    private ?Executable $compare_executable = null;

    #[ORM\ManyToOne(inversedBy: 'problems_run')]
    #[ORM\JoinColumn(name: 'special_run', referencedColumnName: 'execid', onDelete: 'SET NULL')]
    #[Serializer\Exclude]
    private ?Executable $run_executable = null;

    /**
     * @var Collection<int, Testcase>
     */
    #[ORM\OneToMany(mappedBy: 'problem', targetEntity: Testcase::class)]
    #[ORM\OrderBy(['ranknumber' => 'ASC'])]
    #[Serializer\Exclude]
    private Collection $testcases;

    /**
     * @var Collection<int, ProblemStatementContent>
     *
     * We use a OneToMany instead of a OneToOne here, because otherwise this
     * relation will always be loaded. See the commit message of commit
     * 9e421f96691ec67ed62767fe465a6d8751edd884 for a more elaborate explanation
     */
    #[ORM\OneToMany(
        mappedBy: 'problem',
        targetEntity: ProblemStatementContent::class,
        cascade: ['persist'],
        orphanRemoval: true
    )]
    #[Serializer\Exclude]
    private Collection $problemStatementContent;

    /**
     * @var Collection<int, ProblemAttachment>
     */
    #[ORM\OneToMany(mappedBy: 'problem', targetEntity: ProblemAttachment::class, orphanRemoval: true)]
    #[ORM\OrderBy(['name' => 'ASC'])]
    #[Serializer\Exclude]
    private Collection $attachments;

    // This field gets filled by the contest problem visitor with a data transfer
    // object that represents the problem statement.
    #[Serializer\Exclude]
    private ?FileWithName $statementForApi = null;


    /**
     * @var Collection<int, Language>
     */
    #[ORM\ManyToMany(targetEntity: Language::class, inversedBy: 'problems')]
    #[ORM\JoinTable(name: 'problemlanguage')]
    #[ORM\JoinColumn(name: 'probid', referencedColumnName: 'probid', onDelete: 'CASCADE')]
    #[ORM\InverseJoinColumn(name: 'langid', referencedColumnName: 'langid', onDelete: 'CASCADE')]
    #[Serializer\Exclude]
    private Collection $languages;

    public function setProbid(int $probid): Problem
    {
        $this->probid = $probid;
        return $this;
    }

    public function getProbid(): ?int
    {
        return $this->probid;
    }

    public function setExternalid(?string $externalid): Problem
    {
        $this->externalid = $externalid;
        return $this;
    }

    public function getExternalid(): ?string
    {
        return $this->externalid;
    }

    public function setName(string $name): Problem
    {
        $this->name = $name;
        return $this;
    }

    public function getName(): ?string
    {
        return $this->name;
    }

    public function getShortDescription() : ?string
    {
        return $this->getName();
    }

    public function setTimelimit(float $timelimit): Problem
    {
        $this->timelimit = $timelimit;
        return $this;
    }

    #[Serializer\VirtualProperty]
    #[Serializer\SerializedName('time_limit')]
    #[Serializer\Type('float')]
    public function getTimelimit(): float
    {
        return Utils::roundedFloat($this->timelimit);
    }

    public function setMemlimit(?int $memlimit): Problem
    {
        $this->memlimit = $memlimit;
        return $this;
    }

    public function getMemlimit(): ?int
    {
        return $this->memlimit;
    }

    public function setOutputlimit(?int $outputlimit): Problem
    {
        $this->outputlimit = $outputlimit;
        return $this;
    }

    public function getOutputlimit(): ?int
    {
        return $this->outputlimit;
    }

    public function setSpecialCompareArgs(?string $specialCompareArgs): Problem
    {
        $this->special_compare_args = $specialCompareArgs;
        return $this;
    }

    public function getSpecialCompareArgs(): ?string
    {
        return $this->special_compare_args;
    }

    /**
     * @param list<string> $types
     */
    public function setTypesAsString(array $types): Problem
    {
        /** @var array<string, int> $stringToTypes */
        $stringToTypes = array_flip($this->typesToString);
        $typeConstants = [];
        foreach ($types as $type) {
            if (!isset($stringToTypes[$type])) {
                throw new Exception("Unknown problem type: '$type', must be one of " . implode(', ', array_keys($stringToTypes)));
            }
            $typeConstants[$type] = $stringToTypes[$type];
        }
        $this->setTypes($typeConstants);

        return $this;
    }

    public function getTypesAsString(): string
    {
        $typeConstants = $this->getTypes();
        $typeStrings = [];
        foreach ($typeConstants as $type) {
            if (!isset($this->typesToString[$type])) {
                throw new Exception("Unknown problem type: '$type'");
            }
            $typeStrings[] = $this->typesToString[$type];
        }
        return implode(', ', $typeStrings);
    }

    /**
     * @return list<int>
     */
    public function getTypes(): array
    {
        $ret = [];
        foreach (array_keys($this->typesToString) as $type) {
            if ($this->types & $type) {
                $ret[] = $type;
            }
        }
        return $ret;
    }

    /**
     * @param array<int> $types
     */
    public function setTypes(array $types): Problem
    {
        $types = array_unique($types);
        $this->types = 0;
        foreach ($types as $type) {
            $this->types |= $type;
        }
        if (!($this->types & self::TYPE_SCORING)) {
            // In case the problem is not explicitly a scoring problem, default to pass-fail.
            $this->types |= self::TYPE_PASS_FAIL;
        }
        if (($this->types & self::TYPE_PASS_FAIL) && ($this->types & self::TYPE_SCORING)) {
            throw new Exception("Invalid problem type: must be exactly one of 'pass-fail' or 'scoring'.");
        }
        if ($this->types & self::TYPE_SUBMIT_ANSWER) {
            if ($this->types & self::TYPE_MULTI_PASS) {
                throw new Exception("Invalid problem type: 'submit-answer' and 'multi-pass' are mutually exclusive.");
            }
            if ($this->types & self::TYPE_INTERACTIVE) {
                throw new Exception("Invalid problem type: 'submit-answer' and 'interactive' are mutually exclusive.");
            }
        }
        return $this;
    }

    public function isInteractiveProblem(): bool
    {
        return (bool)($this->types & self::TYPE_INTERACTIVE);
    }

    public function isMultipassProblem(): bool
    {
        return (bool)($this->types & self::TYPE_MULTI_PASS);
    }

    public function isPassFailProblem(): bool
    {
        return (bool)($this->types & self::TYPE_PASS_FAIL);
    }

    public function isScoringProblem(): bool
    {
        return (bool)($this->types & self::TYPE_SCORING);
    }

    public function setMultipassLimit(?int $multipassLimit): Problem
    {
        $this->multipassLimit = $multipassLimit;
        return $this;
    }

    public function getMultipassLimit(): int
    {
        if ($this->isMultipassProblem()) {
            return $this->multipassLimit ?? 2;
        }
        return 1;
    }

    public function setProblemstatementFile(?UploadedFile $problemstatementFile): Problem
    {
        $this->problemstatementFile = $problemstatementFile;

        // Clear the problem statement to make sure the entity is modified.
        $this->setProblemStatementContent(null);

        return $this;
    }

    public function setClearProblemstatement(bool $clearProblemstatement): Problem
    {
        $this->clearProblemstatement = $clearProblemstatement;
        $this->setProblemStatementContent(null);

        return $this;
    }

    public function getProblemstatement(): ?string
    {
        return $this->getProblemStatementContent()?->getContent();
    }

    public function getProblemstatementFile(): ?UploadedFile
    {
        return $this->problemstatementFile;
    }

    public function isClearProblemstatement(): bool
    {
        return $this->clearProblemstatement;
    }

    public function setProblemstatementType(?string $problemstatementType): Problem
    {
        $this->problemstatement_type = $problemstatementType;
        return $this;
    }

    public function getProblemstatementType(): ?string
    {
        return $this->problemstatement_type;
    }

    public function setCompareExecutable(?Executable $compareExecutable = null): Problem
    {
        $this->compare_executable = $compareExecutable;
        return $this;
    }

    public function getCompareExecutable(): ?Executable
    {
        return $this->compare_executable;
    }

    public function setRunExecutable(?Executable $runExecutable = null): Problem
    {
        $this->run_executable = $runExecutable;
        return $this;
    }

    public function getRunExecutable(): ?Executable
    {
        return $this->run_executable;
    }

    public function __construct()
    {
        $this->testcases          = new ArrayCollection();
        $this->submissions        = new ArrayCollection();
        $this->clarifications     = new ArrayCollection();
        $this->contest_problems   = new ArrayCollection();
        $this->attachments        = new ArrayCollection();
        $this->languages          = new ArrayCollection();
        $this->problemStatementContent = new ArrayCollection();
    }

    public function addTestcase(Testcase $testcase): Problem
    {
        $this->testcases[] = $testcase;
        return $this;
    }

    /**
     * @return Collection<int, Testcase>
     */
    public function getTestcases(): Collection
    {
        return $this->testcases;
    }

    public function addContestProblem(ContestProblem $contestProblem): Problem
    {
        $this->contest_problems[] = $contestProblem;
        return $this;
    }

    /**
     * @return Collection<int, ContestProblem>
     */
    public function getContestProblems(): Collection
    {
        return $this->contest_problems;
    }

    public function addSubmission(Submission $submission): Problem
    {
        $this->submissions[] = $submission;
        return $this;
    }

    /**
     * @return Collection<int, Submission>
     */
    public function getSubmissions(): Collection
    {
        return $this->submissions;
    }

    public function addClarification(Clarification $clarification): Problem
    {
        $this->clarifications[] = $clarification;
        return $this;
    }

    /**
     * @return Collection<int, Clarification>
     */
    public function getClarifications(): Collection
    {
        return $this->clarifications;
    }

    public function addAttachment(ProblemAttachment $attachment): self
    {
        if (!$this->attachments->contains($attachment)) {
            $this->attachments[] = $attachment;
            $attachment->setProblem($this);
        }

        return $this;
    }

    public function removeAttachment(ProblemAttachment $attachment): self
    {
        if ($this->attachments->contains($attachment)) {
            $this->attachments->removeElement($attachment);
            // set the owning side to null (unless already changed)
            if ($attachment->getProblem() === $this) {
                $attachment->setProblem(null);
            }
        }

        return $this;
    }

    /**
     * @return Collection<int, ProblemAttachment>
     */
    public function getAttachments(): Collection
    {
        return $this->attachments;
    }

    public function setProblemStatementContent(?ProblemStatementContent $content): self
    {
        $this->problemStatementContent = new ArrayCollection();
        if ($content) {
            $this->problemStatementContent->add($content);
            $content->setProblem($this);
        }

        return $this;
    }

    public function getProblemStatementContent(): ?ProblemStatementContent
    {
        return $this->problemStatementContent->first() ?: null;
    }

    #[ORM\PrePersist]
    #[ORM\PreUpdate]
    public function processProblemStatement(): void
    {
        if ($this->isClearProblemstatement()) {
            $this
                ->setProblemStatementContent(null)
                ->setProblemstatementType(null);
        } elseif ($this->getProblemstatementFile()) {
            $content              = file_get_contents($this->getProblemstatementFile()->getRealPath());
            $clientName           = $this->getProblemstatementFile()->getClientOriginalName();
            $problemStatementType = Utils::getTextType($clientName, $this->getProblemstatementFile()->getRealPath());

            if (!isset($problemStatementType)) {
                throw new Exception('Problem statement has unknown file type.');
            }

            $problemStatementContent = (new ProblemStatementContent())
                ->setContent($content);
            $this
                ->setProblemStatementContent($problemStatementContent)
                ->setProblemstatementType($problemStatementType);
        }
    }

    public function getProblemStatementStreamedResponse(): StreamedResponse
    {
        return Utils::getTextStreamedResponse(
            $this->getProblemstatementType(),
            new BadRequestHttpException(sprintf('Problem p%d statement has unknown type', $this->getProbid())),
            sprintf('prob-%s.%s', $this->getName(), $this->getProblemstatementType()),
            $this->getProblemstatement()
        );
    }

    public function setStatementForApi(?FileWithName $statementForApi = null): void
    {
        $this->statementForApi = $statementForApi;
    }

    /**
     * @return FileWithName[]
     */
    #[Serializer\VirtualProperty]
    #[Serializer\SerializedName('statement')]
    #[Serializer\Type('array<App\DataTransferObject\FileWithName>')]
    public function getStatementForApi(): array
    {
        return array_filter([$this->statementForApi]);
    }

    public function addLanguage(Language $language): Problem
    {
        $this->languages[] = $language;
        return $this;
    }

    /**
     * @return Collection<int, Language>
     */
    public function getLanguages(): Collection
    {
        return $this->languages;
    }

    public function removeLanguage(Language $language): Problem
    {
        $this->languages->removeElement($language);
        return $this;
    }
}
